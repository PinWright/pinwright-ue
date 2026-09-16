// Copyright (c) 2026 Alexander Penkin. MIT License.

// Unit tests for UI domain handlers (UiHandler, WidgetAnimationHandler, WidgetUnifiedHandler,
// WidgetHierarchyHandler, WidgetCreateHandler, WidgetTemplateHandler)
#include "Misc/AutomationTest.h"
#include "Dispatch/RpcDispatcher.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/HandlerRegistration.h"
#include "Handlers/ParamSpec.h"
#include "Dom/JsonObject.h"
#include "Tests/TestUtils.h"
#include "EditorAssetLibrary.h"
#include "WidgetBlueprint.h"
#include "Blueprint/WidgetTree.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "HAL/FileManager.h"
#include "UObject/Package.h"
#include "K2Node_ComponentBoundEvent.h"
#include "EdGraph/EdGraph.h"
#include "Compat/EngineVersionCompat.h"
#if WITH_DEV_AUTOMATION_TESTS
#include "Handlers/UI/WidgetHierarchyTestHooks.h"
#endif
#include "Handlers/UI/WidgetAuthoringUtils.h"
#include "Handlers/ErrorCodes.h"

// ============================================================================
// Required-param gate, observed where it actually lives
// ============================================================================
// The `*MissingRequiredParamTest` cases in this file call InvokeHandler(), which
// invokes Reg.Func(Ctx) directly and never runs FRpcDispatcher::ValidateHandlerParams
// (RpcDispatcher.cpp) — the code that enforces RPC_PARAM_REQ. They also assert nothing
// about the response, only that the method is registered, so flipping a required param
// to RPC_PARAM_OPT changes nothing they observe. This helper routes a request through a
// real dispatcher so the MISSING_REQUIRED_PARAM contract is observable; it is the shape
// the rest of those tests should be converted to.
// Named FUiDispatcherValidationResult (not the generic name used by the sibling helper
// in Tests/Assets/TestMaterialHandlers.cpp) so a Unity-merged build cannot hit an ODR
// collision between the two anonymous namespaces.
namespace
{
struct FUiDispatcherValidationResult
{
    bool bCompletionFired = false;
    bool bSuccess = false;
    FString Message;
    FString ErrorCode;
};

FUiDispatcherValidationResult DispatchUiRequestViaDispatcher(
    const FString& RequestId, const FString& MethodName, const TSharedPtr<FJsonObject>& Payload)
{
    FUiDispatcherValidationResult Result;
    FRpcDispatcher Dispatcher;
    // Initialize BEFORE draining so the bridge lambdas capture a live sink.
    Dispatcher.Initialize(FResponseSink(
        [&Result](const FString&, bool bInSuccess, const FString& InMessage,
                  const TSharedPtr<FJsonObject>&, const FString& InErrorCode)
        {
            Result.bCompletionFired = true;
            Result.bSuccess = bInSuccess;
            Result.Message = InMessage;
            Result.ErrorCode = InErrorCode;
        }));
    Dispatcher.DrainAutoRegistrations(nullptr);
    Dispatcher.ProcessRequest(RequestId, MethodName, Payload.IsValid() ? Payload : MakeShared<FJsonObject>());
    return Result;
}

}  // namespace

namespace
{
    FString MakeUniqueWidgetAssetPath(const FString& Prefix);
    void CleanupWidgetAsset(const FString& PackagePath);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUiCreateHudRejectsMissingWidgetPathViaDispatcherTest,
    "PinWright.ui.create_hud.RejectsMissingWidgetPathViaDispatcher",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FUiCreateHudRejectsMissingWidgetPathViaDispatcherTest::RunTest(const FString& Parameters)
{
    // ui.create_hud declares widgetPath as RPC_PARAM_REQ (UiHandler.cpp). The gate that
    // enforces it is in the dispatcher, so an empty payload must come back
    // MISSING_REQUIRED_PARAM without the handler body ever running. Demote the param to
    // RPC_PARAM_OPT and this goes red; FUiCreateHudMissingRequiredParamTest below does not.
    const FUiDispatcherValidationResult Result = DispatchUiRequestViaDispatcher(
        TEXT("req-ui-create-hud-missing"), TEXT("ui.create_hud"), MakeShared<FJsonObject>());

    TestTrue(TEXT("completion fired"), Result.bCompletionFired);
    TestEqual(TEXT("missing widgetPath rejected by the dispatcher"),
        Result.ErrorCode, FString(TEXT("MISSING_REQUIRED_PARAM")));
    return true;
}

// ============================================================================
// UiHandler — ui.screenshot (all-optional params)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUiScreenshotValidParamsNoCrashTest,
    "PinWright.ui.screenshot.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FUiScreenshotValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TestTrue(TEXT("ui.screenshot found"), InvokeHandler(TEXT("ui.screenshot"), MakeShared<FJsonObject>()));
    return true;
}

// ============================================================================
// UiHandler — ui.create_hud
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUiCreateHudValidParamsNoCrashTest,
    "PinWright.ui.create_hud.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FUiCreateHudValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("widgetPath"), TEXT("/Game/UI/TestHUD.TestHUD_C"));
    TestTrue(TEXT("ui.create_hud found"), InvokeHandler(TEXT("ui.create_hud"), Payload));
    return true;
}

// ============================================================================
// UiHandler — ui.set_widget_text
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUiSetWidgetTextValidParamsNoCrashTest,
    "PinWright.ui.set_widget_text.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FUiSetWidgetTextValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("key"), TEXT("MyTextBlock"));
    Payload->SetStringField(TEXT("value"), TEXT("Hello World"));
    TestTrue(TEXT("ui.set_widget_text found"), InvokeHandler(TEXT("ui.set_widget_text"), Payload));
    return true;
}

// ============================================================================
// UiHandler — ui.set_widget_image
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUiSetWidgetImageValidParamsNoCrashTest,
    "PinWright.ui.set_widget_image.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FUiSetWidgetImageValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("key"), TEXT("MyImage"));
    Payload->SetStringField(TEXT("texturePath"), TEXT("/Game/Textures/TestTex"));
    TestTrue(TEXT("ui.set_widget_image found"), InvokeHandler(TEXT("ui.set_widget_image"), Payload));
    return true;
}

// ============================================================================
// UiHandler — ui.set_widget_visibility
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUiSetWidgetVisibilityValidParamsNoCrashTest,
    "PinWright.ui.set_widget_visibility.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FUiSetWidgetVisibilityValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("key"), TEXT("MyWidget"));
    Payload->SetBoolField(TEXT("visible"), false);
    TestTrue(TEXT("ui.set_widget_visibility found"), InvokeHandler(TEXT("ui.set_widget_visibility"), Payload));
    return true;
}

// Regression for E-set-widget-visibility-bool-loses-eslatevisibility:
// ui.set_widget_visibility must accept the full ESlateVisibility enum via a
// `visibility` string param (not only the Visible/Collapsed bool), and must
// reject an unknown value rather than silently coercing it to false. The bool-
// only schema this replaced declared exactly 2 params (key, visible) — this
// asserts the `visibility` param is now part of the registered contract.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUiSetWidgetVisibilityEnumParamRegisteredTest,
    "PinWright.ui.set_widget_visibility.EnumParamRegistered",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FUiSetWidgetVisibilityEnumParamRegisteredTest::RunTest(const FString& Parameters)
{
    bool bFound = false;
    for (const FHandlerRegistration& Reg : FAutoRegisterHandler::GetPendingRegistrations())
    {
        if (Reg.MethodName != TEXT("ui.set_widget_visibility")) continue;
        bFound = true;

        bool bHasKey = false;
        bool bHasVisibilityEnum = false;
        bool bHasVisibleBool = false;
        for (const FParamSpec& Param : Reg.Params)
        {
            if (Param.Name == TEXT("key")) bHasKey = true;
            else if (Param.Name == TEXT("visibility"))
            {
                bHasVisibilityEnum = true;
                TestEqual(TEXT("visibility param is a string"), Param.Type, FString(TEXT("string")));
            }
            else if (Param.Name == TEXT("visible")) bHasVisibleBool = true;
        }
        TestTrue(TEXT("key param present"), bHasKey);
        TestTrue(TEXT("visibility enum param present (the fix's new contract)"), bHasVisibilityEnum);
        TestTrue(TEXT("visible bool retained for back-compat"), bHasVisibleBool);
        break;
    }
    TestTrue(TEXT("ui.set_widget_visibility registered"), bFound);
    return true;
}

// Exercises the production parse/format helpers the handler now wires in. These
// are the exact functions ui.set_widget_visibility resolves `visibility` through,
// so this fails if the enum table or the strict-reject contract regresses. The
// three states unreachable through the old bool (Hidden / HitTestInvisible /
// SelfHitTestInvisible) are asserted explicitly.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUiSetWidgetVisibilityEnumRoundTripTest,
    "PinWright.ui.set_widget_visibility.EnumRoundTrip",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FUiSetWidgetVisibilityEnumRoundTripTest::RunTest(const FString& Parameters)
{
    struct FCase { const TCHAR* Name; ESlateVisibility Value; };
    const FCase Cases[] = {
        { TEXT("Visible"),              ESlateVisibility::Visible },
        { TEXT("Collapsed"),            ESlateVisibility::Collapsed },
        { TEXT("Hidden"),               ESlateVisibility::Hidden },
        { TEXT("HitTestInvisible"),     ESlateVisibility::HitTestInvisible },
        { TEXT("SelfHitTestInvisible"), ESlateVisibility::SelfHitTestInvisible },
    };
    for (const FCase& C : Cases)
    {
        ESlateVisibility Parsed = ESlateVisibility::Visible;
        TestTrue(*FString::Printf(TEXT("parse %s succeeds"), C.Name),
            WidgetAuthoringHelpers::TryParseVisibility(C.Name, Parsed));
        TestTrue(*FString::Printf(TEXT("parse %s -> correct enum"), C.Name),
            Parsed == C.Value);
        TestEqual(*FString::Printf(TEXT("%s round-trips through VisibilityToString"), C.Name),
            WidgetAuthoringHelpers::VisibilityToString(C.Value), FString(C.Name));
    }

    // Case-insensitive parse (the wire may carry any casing).
    {
        ESlateVisibility Parsed = ESlateVisibility::Visible;
        TestTrue(TEXT("lowercase 'collapsed' parses"),
            WidgetAuthoringHelpers::TryParseVisibility(TEXT("collapsed"), Parsed));
        TestTrue(TEXT("lowercase 'collapsed' -> Collapsed"),
            Parsed == ESlateVisibility::Collapsed);
    }

    // An unknown string must be rejected (left untouched), NOT silently coerced —
    // the precise defect this ticket fixed (old path ran it through GetBool->false).
    {
        ESlateVisibility Parsed = ESlateVisibility::Hidden; // sentinel, must survive
        TestFalse(TEXT("unknown 'Nope' is rejected"),
            WidgetAuthoringHelpers::TryParseVisibility(TEXT("Nope"), Parsed));
        TestTrue(TEXT("rejected parse leaves Out untouched"),
            Parsed == ESlateVisibility::Hidden);
    }
    return true;
}

// Same ticket (E-set-widget-visibility-bool-loses-eslatevisibility), but asserted
// THROUGH the handler rather than around it. The two tests above observe only the
// registered ParamSpec and the free helpers; both stay green if the handler body's
// visibility branch (UiHandler.cpp: the `visibility` string read, the
// TryParseVisibility call and the INVALID_VISIBILITY reject) is deleted and the
// old `Ctx.GetBool("visible")`-only path restored — the param would still be
// declared and the helpers would still parse, just with nothing calling them.
//
// The reject happens BEFORE the runtime widget lookup, so it is observable headless
// with no PIE: an unparseable `visibility` must come back INVALID_VISIBILITY, while
// a valid one must get past the parse and fail later (WIDGET_NOT_FOUND). That pair
// is what discriminates "the handler consults the enum" from "the handler ignores it".
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUiSetWidgetVisibilityHandlerConsultsEnumTest,
    "PinWright.ui.set_widget_visibility.HandlerConsultsEnum",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FUiSetWidgetVisibilityHandlerConsultsEnumTest::RunTest(const FString& Parameters)
{
    // A key no live widget can carry, so the outcome is decided by the visibility
    // parse alone and never by an incidental viewport match.
    const FString UnmatchableKey = FString::Printf(
        TEXT("PW_NoSuchWidget_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));

    // Unknown enum string: rejected at the parse, before the widget lookup.
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("key"), UnmatchableKey);
        Payload->SetStringField(TEXT("visibility"), TEXT("Nope"));
        FTestResponseCapture Capture;
        TestTrue(TEXT("ui.set_widget_visibility found (unknown enum)"),
            InvokeHandlerWithCapture(TEXT("ui.set_widget_visibility"), Payload, Capture));
        TestFalse(TEXT("unknown visibility is not a success"), Capture.bSuccess);
        TestEqual(TEXT("unknown visibility rejected by the handler, not coerced to a bool"),
            Capture.ErrorCode, FString(TEXT("INVALID_VISIBILITY")));
    }

    // Valid enum string the old bool path could not express: must get PAST the parse
    // and fail on the (absent) live widget instead.
    for (const TCHAR* Valid : { TEXT("Hidden"), TEXT("HitTestInvisible"), TEXT("SelfHitTestInvisible") })
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("key"), UnmatchableKey);
        Payload->SetStringField(TEXT("visibility"), Valid);
        FTestResponseCapture Capture;
        TestTrue(*FString::Printf(TEXT("ui.set_widget_visibility found (%s)"), Valid),
            InvokeHandlerWithCapture(TEXT("ui.set_widget_visibility"), Payload, Capture));
        TestNotEqual(*FString::Printf(TEXT("'%s' parses through the handler"), Valid),
            Capture.ErrorCode, FString(TEXT("INVALID_VISIBILITY")));
        TestEqual(*FString::Printf(TEXT("'%s' reaches the widget lookup"), Valid),
            Capture.ErrorCode, FString(TEXT("WIDGET_NOT_FOUND")));
    }
    return true;
}

// ============================================================================
// UiHandler — ui.remove_widget_from_viewport (all-optional params)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUiRemoveWidgetFromViewportNoCrashTest,
    "PinWright.ui.remove_widget_from_viewport.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FUiRemoveWidgetFromViewportNoCrashTest::RunTest(const FString& Parameters)
{
    TestTrue(TEXT("ui.remove_widget_from_viewport found"), InvokeHandler(TEXT("ui.remove_widget_from_viewport"), MakeShared<FJsonObject>()));
    return true;
}

// ============================================================================
// WidgetAnimationHandler — widget.create_widget_animation
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWidgetCreateWidgetAnimationValidParamsNoCrashTest,
    "PinWright.widget.create_widget_animation.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWidgetCreateWidgetAnimationValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("widgetPath"), TEXT("/Game/UI/TestWidget"));
    Payload->SetStringField(TEXT("animationName"), TEXT("FadeIn"));
    TestTrue(TEXT("widget.create_widget_animation found"), InvokeHandler(TEXT("widget.create_widget_animation"), Payload));
    return true;
}

// ============================================================================
// WidgetAnimationHandler — widget.add_animation_track
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWidgetAddAnimationTrackValidParamsNoCrashTest,
    "PinWright.widget.add_animation_track.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWidgetAddAnimationTrackValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("widgetPath"), TEXT("/Game/UI/TestWidget"));
    Payload->SetStringField(TEXT("animationName"), TEXT("FadeIn"));
    Payload->SetStringField(TEXT("widgetName"), TEXT("MyPanel"));
    TestTrue(TEXT("widget.add_animation_track found"), InvokeHandler(TEXT("widget.add_animation_track"), Payload));
    return true;
}

// ============================================================================
// WidgetAnimationHandler — widget.add_animation_keyframe
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWidgetAddAnimationKeyframeValidParamsNoCrashTest,
    "PinWright.widget.add_animation_keyframe.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWidgetAddAnimationKeyframeValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("widgetPath"), TEXT("/Game/UI/TestWidget"));
    Payload->SetStringField(TEXT("animationName"), TEXT("FadeIn"));
    Payload->SetNumberField(TEXT("time"), 0.5);
    Payload->SetNumberField(TEXT("value"), 0.0);
    TestTrue(TEXT("widget.add_animation_keyframe found"), InvokeHandler(TEXT("widget.add_animation_keyframe"), Payload));
    return true;
}

// ============================================================================
// WidgetAnimationHandler — widget.set_animation_loop
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWidgetSetAnimationLoopValidParamsNoCrashTest,
    "PinWright.widget.set_animation_loop.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWidgetSetAnimationLoopValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("widgetPath"), TEXT("/Game/UI/TestWidget"));
    Payload->SetStringField(TEXT("animationName"), TEXT("Idle"));
    Payload->SetBoolField(TEXT("loop"), true);
    TestTrue(TEXT("widget.set_animation_loop found"), InvokeHandler(TEXT("widget.set_animation_loop"), Payload));
    return true;
}

// ============================================================================
// WidgetAnimationHandler — widget.set_animation_speed
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWidgetSetAnimationSpeedValidParamsNoCrashTest,
    "PinWright.widget.set_animation_speed.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWidgetSetAnimationSpeedValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("widgetPath"), TEXT("/Game/UI/TestWidget"));
    Payload->SetStringField(TEXT("animationName"), TEXT("FadeIn"));
    Payload->SetNumberField(TEXT("speed"), 2.0);
    TestTrue(TEXT("widget.set_animation_speed found"), InvokeHandler(TEXT("widget.set_animation_speed"), Payload));
    return true;
}

// ============================================================================
// WidgetAnimationHandler — widget.get_animation_info
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWidgetGetAnimationInfoValidParamsNoCrashTest,
    "PinWright.widget.get_animation_info.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWidgetGetAnimationInfoValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("widgetPath"), TEXT("/Game/UI/TestWidget"));
    TestTrue(TEXT("widget.get_animation_info found"), InvokeHandler(TEXT("widget.get_animation_info"), Payload));
    return true;
}

// ============================================================================
// WidgetAnimationHandler — widget.delete_animation
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWidgetDeleteAnimationValidParamsNoCrashTest,
    "PinWright.widget.delete_animation.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWidgetDeleteAnimationValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("widgetPath"), TEXT("/Game/UI/TestWidget"));
    Payload->SetStringField(TEXT("animationName"), TEXT("FadeIn"));
    TestTrue(TEXT("widget.delete_animation found"), InvokeHandler(TEXT("widget.delete_animation"), Payload));
    return true;
}

// ============================================================================
// WidgetUnifiedHandler — widget.add (unified replacement for all widget.add_* methods)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWidgetAddValidParamsNoCrashTest,
    "PinWright.widget.add.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWidgetAddValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("widgetPath"), TEXT("/Game/UI/NonExistentWidget"));
    Payload->SetStringField(TEXT("type"), TEXT("TextBlock"));
    Payload->SetStringField(TEXT("name"), TEXT("MyText"));
    TestTrue(TEXT("widget.add found"), InvokeHandler(TEXT("widget.add"), Payload));
    return true;
}

// ============================================================================
// WidgetUnifiedHandler — widget.set (unified replacement for all widget.set_* methods)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWidgetSetValidParamsNoCrashTest,
    "PinWright.widget.set.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWidgetSetValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("widgetPath"), TEXT("/Game/UI/NonExistentWidget"));
    Payload->SetStringField(TEXT("widgetName"), TEXT("MyButton"));
    TSharedPtr<FJsonObject> Props = MakeShared<FJsonObject>();
    Props->SetStringField(TEXT("Visibility"), TEXT("Collapsed"));
    Payload->SetObjectField(TEXT("properties"), Props);
    TestTrue(TEXT("widget.set found"), InvokeHandler(TEXT("widget.set"), Payload));
    return true;
}

// ============================================================================
// WidgetUnifiedHandler — widget.bind (unified replacement for all widget.bind_* methods)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWidgetBindValidParamsNoCrashTest,
    "PinWright.widget.bind.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWidgetBindValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("widgetPath"), TEXT("/Game/UI/NonExistentWidget"));
    Payload->SetStringField(TEXT("widgetName"), TEXT("HealthBar"));
    Payload->SetStringField(TEXT("propertyName"), TEXT("Percent"));
    Payload->SetStringField(TEXT("functionName"), TEXT("GetHealthPercent"));
    TestTrue(TEXT("widget.bind found"), InvokeHandler(TEXT("widget.bind"), Payload));
    return true;
}

// ============================================================================
// WidgetCreateHandler — widget.create_widget_blueprint
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWidgetCreateWidgetBlueprintValidParamsNoCrashTest,
    "PinWright.widget.create_widget_blueprint.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWidgetCreateWidgetBlueprintValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("name"), TEXT("WBP_TestUnit"));
    TestTrue(TEXT("widget.create_widget_blueprint found"), InvokeHandler(TEXT("widget.create_widget_blueprint"), Payload));
    CleanupTestAsset(TEXT("/Game/UI/WBP_TestUnit"));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWidgetCreateWidgetBlueprintSaveWritesToDiskTest,
    "PinWright.widget.create_widget_blueprint.SaveWritesToDisk",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWidgetCreateWidgetBlueprintSaveWritesToDiskTest::RunTest(const FString& Parameters)
{
    const FString AssetPath = MakeUniqueWidgetAssetPath(TEXT("WBP_Save"));
    const FString AssetName = FPackageName::GetLongPackageAssetName(AssetPath);
    const FString Folder = AssetPath.Left(AssetPath.Len() - AssetName.Len() - 1);

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("name"), AssetName);
    Payload->SetStringField(TEXT("folder"), Folder);
    Payload->SetStringField(TEXT("parentClass"), TEXT("UserWidget"));

    FTestResponseCapture Capture;
    TestTrue(TEXT("widget.create_widget_blueprint handler found"),
        InvokeHandlerWithCapture(TEXT("widget.create_widget_blueprint"), Payload, Capture));
    TestTrue(TEXT("widget.create_widget_blueprint responded"), Capture.bWasCalled);
    TestTrue(TEXT("widget.create_widget_blueprint succeeded"), Capture.bSuccess);

    if (Capture.bSuccess && Capture.Result.IsValid())
    {
        FString ObjectPath;
        Capture.Result->TryGetStringField(TEXT("widgetPath"), ObjectPath);
        TestTrue(TEXT("created widget is registry-visible"),
            !ObjectPath.IsEmpty() && UEditorAssetLibrary::DoesAssetExist(ObjectPath));
        TestTrue(TEXT("created widget .uasset is on disk"),
            IFileManager::Get().FileSize(*PackageFilenameFromAssetPath(ObjectPath)) > 0);

        FString ActualParentClass;
        TestTrue(TEXT("response reports the actual parent class"),
            Capture.Result->TryGetStringField(TEXT("parentClass"), ActualParentClass)
            && !ActualParentClass.IsEmpty());
        TestEqual(TEXT("response reads back the canonical UUserWidget parent"),
            ActualParentClass, UUserWidget::StaticClass()->GetPathName());

        bool bSaveRequested = false;
        bool bSaved = false;
        TestTrue(TEXT("saveRequested is reported true"),
            Capture.Result->TryGetBoolField(TEXT("saveRequested"), bSaveRequested) && bSaveRequested);
        TestTrue(TEXT("saved is reported true"),
            Capture.Result->TryGetBoolField(TEXT("saved"), bSaved) && bSaved);
        TestFalse(TEXT("pendingFlush is absent after a durable save"),
            Capture.Result->HasField(TEXT("pendingFlush")));
    }

    CleanupWidgetAsset(AssetPath);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWidgetCreateWidgetBlueprintSaveFalseIsMemoryOnlyTest,
    "PinWright.widget.create_widget_blueprint.SaveFalseIsMemoryOnly",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWidgetCreateWidgetBlueprintSaveFalseIsMemoryOnlyTest::RunTest(const FString& Parameters)
{
    const FString AssetPath = MakeUniqueWidgetAssetPath(TEXT("WBP_SaveFalse"));
    const FString AssetName = FPackageName::GetLongPackageAssetName(AssetPath);
    const FString Folder = AssetPath.Left(AssetPath.Len() - AssetName.Len() - 1);

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("name"), AssetName);
    Payload->SetStringField(TEXT("folder"), Folder);
    Payload->SetBoolField(TEXT("save"), false);

    FTestResponseCapture Capture;
    TestTrue(TEXT("widget.create_widget_blueprint handler found"),
        InvokeHandlerWithCapture(TEXT("widget.create_widget_blueprint"), Payload, Capture));
    TestTrue(TEXT("widget.create_widget_blueprint responded"), Capture.bWasCalled);
    TestTrue(TEXT("widget.create_widget_blueprint succeeded"), Capture.bSuccess);

    if (Capture.bSuccess && Capture.Result.IsValid())
    {
        FString ObjectPath;
        Capture.Result->TryGetStringField(TEXT("widgetPath"), ObjectPath);
        TestTrue(TEXT("save:false asset remains registry-visible"),
            !ObjectPath.IsEmpty() && UEditorAssetLibrary::DoesAssetExist(ObjectPath));
        bool bSaveRequested = true;
        bool bSaved = true;
        TestTrue(TEXT("save:false reports saveRequested:false"),
            Capture.Result->TryGetBoolField(TEXT("saveRequested"), bSaveRequested) && !bSaveRequested);
        TestTrue(TEXT("save:false reports saved:false"),
            Capture.Result->TryGetBoolField(TEXT("saved"), bSaved) && !bSaved);

        FString SaveState;
        TestTrue(TEXT("save:false reports saveState:notRequested"),
            Capture.Result->TryGetStringField(TEXT("saveState"), SaveState)
            && SaveState == TEXT("notRequested"));
        TestFalse(TEXT("save:false does not report pendingFlush"),
            Capture.Result->HasField(TEXT("pendingFlush")));

        bool bPendingSave = false;
        TestTrue(TEXT("save:false reports a pending dirty package"),
            Capture.Result->TryGetBoolField(TEXT("pendingSave"), bPendingSave) && bPendingSave);
        TestTrue(TEXT("save:false package is dirty"),
            FindPackage(nullptr, *AssetPath) != nullptr
            && FindPackage(nullptr, *AssetPath)->IsDirty());
        TestTrue(TEXT("save:false writes no .uasset"),
            IFileManager::Get().FileSize(*PackageFilenameFromAssetPath(AssetPath)) < 0);
    }

    if (UPackage* Package = FindPackage(nullptr, *AssetPath))
    {
        Package->SetDirtyFlag(false);
    }
    CleanupWidgetAsset(AssetPath);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWidgetCreateWidgetBlueprintInvalidParentRefusedTest,
    "PinWright.widget.create_widget_blueprint.InvalidParentRefused",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWidgetCreateWidgetBlueprintInvalidParentRefusedTest::RunTest(const FString& Parameters)
{
    const FString AssetPath = MakeUniqueWidgetAssetPath(TEXT("WBP_InvalidParent"));
    const FString AssetName = FPackageName::GetLongPackageAssetName(AssetPath);
    const FString Folder = AssetPath.Left(AssetPath.Len() - AssetName.Len() - 1);

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("name"), AssetName);
    Payload->SetStringField(TEXT("folder"), Folder);
    Payload->SetStringField(TEXT("parentClass"),
        FString::Printf(TEXT("PW_NoSuchWidgetParent_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits)));

    FTestResponseCapture Capture;
    TestTrue(TEXT("widget.create_widget_blueprint handler found"),
        InvokeHandlerWithCapture(TEXT("widget.create_widget_blueprint"), Payload, Capture));
    TestTrue(TEXT("widget.create_widget_blueprint responded"), Capture.bWasCalled);
    TestFalse(TEXT("invalid parent is rejected"), Capture.bSuccess);
    TestEqual(TEXT("unresolved parent returns CLASS_NOT_FOUND"),
        Capture.ErrorCode, FString(ErrorCodes::ERR_CLASS_NOT_FOUND));
    TestFalse(TEXT("invalid parent creates no registry asset"),
        UEditorAssetLibrary::DoesAssetExist(AssetPath));
    TestNull(TEXT("invalid parent creates no package"), FindPackage(nullptr, *AssetPath));
    TestTrue(TEXT("invalid parent creates no .uasset"),
        IFileManager::Get().FileSize(*PackageFilenameFromAssetPath(AssetPath)) < 0);

    CleanupWidgetAsset(AssetPath);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWidgetCreateWidgetBlueprintIncompatibleParentRefusedTest,
    "PinWright.widget.create_widget_blueprint.IncompatibleParentRefused",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWidgetCreateWidgetBlueprintIncompatibleParentRefusedTest::RunTest(const FString& Parameters)
{
    const FString AssetPath = MakeUniqueWidgetAssetPath(TEXT("WBP_IncompatibleParent"));
    const FString AssetName = FPackageName::GetLongPackageAssetName(AssetPath);
    const FString Folder = AssetPath.Left(AssetPath.Len() - AssetName.Len() - 1);

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("name"), AssetName);
    Payload->SetStringField(TEXT("folder"), Folder);
    Payload->SetStringField(TEXT("parentClass"), TEXT("/Script/Engine.Actor"));

    FTestResponseCapture Capture;
    TestTrue(TEXT("widget.create_widget_blueprint handler found"),
        InvokeHandlerWithCapture(TEXT("widget.create_widget_blueprint"), Payload, Capture));
    TestTrue(TEXT("widget.create_widget_blueprint responded"), Capture.bWasCalled);
    TestFalse(TEXT("incompatible parent is rejected"), Capture.bSuccess);
    TestEqual(TEXT("incompatible parent returns CLASS_NOT_INSTANTIABLE"),
        Capture.ErrorCode, FString(ErrorCodes::ERR_CLASS_NOT_INSTANTIABLE));
    TestFalse(TEXT("incompatible parent creates no registry asset"),
        UEditorAssetLibrary::DoesAssetExist(AssetPath));
    TestNull(TEXT("incompatible parent creates no package"), FindPackage(nullptr, *AssetPath));
    TestTrue(TEXT("incompatible parent creates no .uasset"),
        IFileManager::Get().FileSize(*PackageFilenameFromAssetPath(AssetPath)) < 0);

    CleanupWidgetAsset(AssetPath);
    return true;
}

// ============================================================================
// WidgetHierarchyHandler — widget.remove_widget
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWidgetRemoveWidgetValidParamsNoCrashTest,
    "PinWright.widget.remove_widget.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWidgetRemoveWidgetValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("widgetPath"), TEXT("/Game/UI/TestWidget"));
    Payload->SetStringField(TEXT("slotName"), TEXT("OldButton"));
    TestTrue(TEXT("widget.remove_widget found"), InvokeHandler(TEXT("widget.remove_widget"), Payload));
    return true;
}

// ============================================================================
// WidgetHierarchyHandler — widget.rename_widget
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWidgetRenameWidgetValidParamsNoCrashTest,
    "PinWright.widget.rename_widget.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWidgetRenameWidgetValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("widgetPath"), TEXT("/Game/UI/TestWidget"));
    Payload->SetStringField(TEXT("slotName"), TEXT("OldName"));
    Payload->SetStringField(TEXT("newName"), TEXT("NewName"));
    TestTrue(TEXT("widget.rename_widget found"), InvokeHandler(TEXT("widget.rename_widget"), Payload));
    return true;
}

// ============================================================================
// WidgetHierarchyHandler — widget.reparent_widget
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWidgetReparentWidgetValidParamsNoCrashTest,
    "PinWright.widget.reparent_widget.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWidgetReparentWidgetValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("widgetPath"), TEXT("/Game/UI/TestWidget"));
    Payload->SetStringField(TEXT("slotName"), TEXT("ChildButton"));
    Payload->SetStringField(TEXT("newParent"), TEXT("ContentBox"));
    TestTrue(TEXT("widget.reparent_widget found"), InvokeHandler(TEXT("widget.reparent_widget"), Payload));
    return true;
}

// ============================================================================
// WidgetClassInspectHandler — widget.get_class_properties
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWidgetGetClassPropertiesValidParamsNoCrashTest,
    "PinWright.widget.get_class_properties.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWidgetGetClassPropertiesValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    TArray<TSharedPtr<FJsonValue>> Classes;
    Classes.Add(MakeShared<FJsonValueString>(TEXT("Button")));
    Classes.Add(MakeShared<FJsonValueString>(TEXT("TextBlock")));
    Payload->SetArrayField(TEXT("classes"), Classes);
    TestTrue(TEXT("widget.get_class_properties found"), InvokeHandler(TEXT("widget.get_class_properties"), Payload));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWidgetGetClassPropertiesEmptyArrayTest,
    "PinWright.widget.get_class_properties.EmptyArray",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWidgetGetClassPropertiesEmptyArrayTest::RunTest(const FString& Parameters)
{
    // Empty classes array — should return error, not crash
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetArrayField(TEXT("classes"), TArray<TSharedPtr<FJsonValue>>());
    TestTrue(TEXT("widget.get_class_properties empty array"), InvokeHandler(TEXT("widget.get_class_properties"), Payload));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWidgetGetClassPropertiesSingleClassTest,
    "PinWright.widget.get_class_properties.SingleClass",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWidgetGetClassPropertiesSingleClassTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    TArray<TSharedPtr<FJsonValue>> Classes;
    Classes.Add(MakeShared<FJsonValueString>(TEXT("ProgressBar")));
    Payload->SetArrayField(TEXT("classes"), Classes);
    TestTrue(TEXT("widget.get_class_properties single class"), InvokeHandler(TEXT("widget.get_class_properties"), Payload));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWidgetGetClassPropertiesNonExistentClassTest,
    "PinWright.widget.get_class_properties.NonExistentClass",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWidgetGetClassPropertiesNonExistentClassTest::RunTest(const FString& Parameters)
{
    // Non-existent class should return empty array for that class, not crash
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    TArray<TSharedPtr<FJsonValue>> Classes;
    Classes.Add(MakeShared<FJsonValueString>(TEXT("ThisWidgetDoesNotExist_XYZ")));
    Payload->SetArrayField(TEXT("classes"), Classes);
    TestTrue(TEXT("widget.get_class_properties non-existent"), InvokeHandler(TEXT("widget.get_class_properties"), Payload));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWidgetGetClassPropertiesMixedValidInvalidTest,
    "PinWright.widget.get_class_properties.MixedValidAndInvalid",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWidgetGetClassPropertiesMixedValidInvalidTest::RunTest(const FString& Parameters)
{
    // Mix of valid and invalid class names — should handle gracefully
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    TArray<TSharedPtr<FJsonValue>> Classes;
    Classes.Add(MakeShared<FJsonValueString>(TEXT("Button")));
    Classes.Add(MakeShared<FJsonValueString>(TEXT("NonExistentWidget_XYZ")));
    Classes.Add(MakeShared<FJsonValueString>(TEXT("Image")));
    Payload->SetArrayField(TEXT("classes"), Classes);
    TestTrue(TEXT("widget.get_class_properties mixed"), InvokeHandler(TEXT("widget.get_class_properties"), Payload));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWidgetGetClassPropertiesCustomMaxDepthTest,
    "PinWright.widget.get_class_properties.CustomMaxDepth",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWidgetGetClassPropertiesCustomMaxDepthTest::RunTest(const FString& Parameters)
{
    // maxDepth=0 to disable struct recursion
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    TArray<TSharedPtr<FJsonValue>> Classes;
    Classes.Add(MakeShared<FJsonValueString>(TEXT("TextBlock")));
    Payload->SetArrayField(TEXT("classes"), Classes);
    Payload->SetNumberField(TEXT("maxDepth"), 0);
    TestTrue(TEXT("widget.get_class_properties depth=0"), InvokeHandler(TEXT("widget.get_class_properties"), Payload));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWidgetGetClassPropertiesDeepMaxDepthTest,
    "PinWright.widget.get_class_properties.DeepMaxDepth",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWidgetGetClassPropertiesDeepMaxDepthTest::RunTest(const FString& Parameters)
{
    // maxDepth=5 to exercise deeper recursion
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    TArray<TSharedPtr<FJsonValue>> Classes;
    Classes.Add(MakeShared<FJsonValueString>(TEXT("Button")));
    Payload->SetArrayField(TEXT("classes"), Classes);
    Payload->SetNumberField(TEXT("maxDepth"), 5);
    TestTrue(TEXT("widget.get_class_properties depth=5"), InvokeHandler(TEXT("widget.get_class_properties"), Payload));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWidgetGetClassPropertiesSlotClassTest,
    "PinWright.widget.get_class_properties.SlotClass",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWidgetGetClassPropertiesSlotClassTest::RunTest(const FString& Parameters)
{
    // Test with a slot class (CanvasPanelSlot) — common use case per plan
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    TArray<TSharedPtr<FJsonValue>> Classes;
    Classes.Add(MakeShared<FJsonValueString>(TEXT("CanvasPanelSlot")));
    Payload->SetArrayField(TEXT("classes"), Classes);
    TestTrue(TEXT("widget.get_class_properties slot class"), InvokeHandler(TEXT("widget.get_class_properties"), Payload));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWidgetGetClassPropertiesParamSpecTest,
    "PinWright.widget.get_class_properties.ParamSpecRegistered",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWidgetGetClassPropertiesParamSpecTest::RunTest(const FString& Parameters)
{
    for (const FHandlerRegistration& Reg : FAutoRegisterHandler::GetPendingRegistrations())
    {
        if (Reg.MethodName == TEXT("widget.get_class_properties"))
        {
            TestEqual(TEXT("param count"), Reg.Params.Num(), 2);
            TestTrue(TEXT("classes param is required"), Reg.Params[0].bRequired);
            TestFalse(TEXT("maxDepth param is optional"), Reg.Params[1].bRequired);
            TestEqual(TEXT("category"), Reg.Category, TEXT("widget"));
            return true;
        }
    }
    AddError(TEXT("widget.get_class_properties not found in registrations"));
    return true;
}

// ============================================================================
// GUID Consistency Tests
// Verify WidgetVariableNameToGuidMap is maintained correctly during mutations
// ============================================================================

namespace
{
    FString MakeUniqueWidgetAssetPath(const FString& Prefix)
    {
        return FString::Printf(
            TEXT("/Game/__PW_GatewayTests/%s_%s"),
            *Prefix,
            *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    }

    FString ToWidgetObjectPath(const FString& PackagePath)
    {
        const FString AssetName = FPackageName::GetLongPackageAssetName(PackagePath);
        return AssetName.IsEmpty()
            ? PackagePath
            : FString::Printf(TEXT("%s.%s"), *PackagePath, *AssetName);
    }

    // Detach rather than force-delete: UEditorAssetLibrary::DeleteAsset routes through
    // ObjectTools::ForceDeleteObjects, whose referencer sweep walks every live UObject; the 10
    // anchors across widget+ui cost 137 s of that namespace pair's 177 s of suite time.
    // A UWidgetBlueprint is a UBlueprint, so CleanupTestAsset -> DiscardLoadedAssetNoGc removes
    // its generated/skeleton classes before renaming it into /Transient, and it deletes the saved
    // .uasset that widget.create_widget_blueprint writes with save:true.
    void CleanupWidgetAsset(const FString& PackagePath)
    {
        if (PackagePath.IsEmpty()) return;
        CleanupTestAsset(PackagePath);
    }

    // Create a widget blueprint via the handler and return the package path
    bool CreateTestWidgetBlueprint(const FString& AssetPath, FTestResponseCapture& Capture)
    {
        const FString AssetName = FPackageName::GetLongPackageAssetName(AssetPath);
        const FString Folder = AssetPath.Left(AssetPath.Len() - AssetName.Len() - 1);

        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("name"), AssetName);
        Payload->SetStringField(TEXT("folder"), Folder);
        return InvokeHandlerWithCapture(TEXT("widget.create_widget_blueprint"), Payload, Capture);
    }

    UWidgetBlueprint* LoadTestWidgetBlueprint(const FString& PackagePath)
    {
        const FString ObjectPath = ToWidgetObjectPath(PackagePath);
        UObject* Loaded = UEditorAssetLibrary::LoadAsset(ObjectPath);
        if (!Loaded) Loaded = UEditorAssetLibrary::LoadAsset(PackagePath);
        return Cast<UWidgetBlueprint>(Loaded);
    }
}

// ---- GuidConsistency.CreateAndAddWidget ----
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGuidConsistencyCreateAndAddWidgetTest,
    "PinWright.widget.GuidConsistency.CreateAndAddWidget",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FGuidConsistencyCreateAndAddWidgetTest::RunTest(const FString& Parameters)
{
    const FString AssetPath = MakeUniqueWidgetAssetPath(TEXT("WBP_GuidCreate"));
    FTestResponseCapture Capture;

    // Create widget blueprint
    TestTrue(TEXT("create handler found"), CreateTestWidgetBlueprint(AssetPath, Capture));
    TestTrue(TEXT("create succeeded"), Capture.bSuccess);

    // Add a canvas panel via unified widget.add
    TSharedPtr<FJsonObject> AddPayload = MakeShared<FJsonObject>();
    AddPayload->SetStringField(TEXT("widgetPath"), AssetPath);
    AddPayload->SetStringField(TEXT("type"), TEXT("CanvasPanel"));
    AddPayload->SetStringField(TEXT("name"), TEXT("TestCanvas"));
    TestTrue(TEXT("widget.add found"), InvokeHandlerWithCapture(TEXT("widget.add"), AddPayload, Capture));
    TestTrue(TEXT("widget.add succeeded"), Capture.bSuccess);

    // Load and verify GUID map
    UWidgetBlueprint* WBP = LoadTestWidgetBlueprint(AssetPath);
    if (TestNotNull(TEXT("widget blueprint loaded"), WBP))
    {
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 6, 0)
        // UE 5.4/5.5 has no WidgetVariableNameToGuidMap; this GUID-map invariant only
        // applies on 5.6+. The handler-success checks above still run on 5.4/5.5.
        TestTrue(TEXT("TestCanvas has GUID entry"),
            WBP->WidgetVariableNameToGuidMap.Contains(FName(TEXT("TestCanvas"))));
#endif
    }

    CleanupWidgetAsset(AssetPath);
    return true;
}

// ---- GuidConsistency.RemoveWidget ----
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGuidConsistencyRemoveWidgetTest,
    "PinWright.widget.GuidConsistency.RemoveWidget",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FGuidConsistencyRemoveWidgetTest::RunTest(const FString& Parameters)
{
    const FString AssetPath = MakeUniqueWidgetAssetPath(TEXT("WBP_GuidRemove"));
    FTestResponseCapture Capture;

    // Create widget blueprint
    TestTrue(TEXT("create handler found"), CreateTestWidgetBlueprint(AssetPath, Capture));
    TestTrue(TEXT("create succeeded"), Capture.bSuccess);

    // Add a canvas panel as root via unified widget.add
    TSharedPtr<FJsonObject> AddPayload = MakeShared<FJsonObject>();
    AddPayload->SetStringField(TEXT("widgetPath"), AssetPath);
    AddPayload->SetStringField(TEXT("type"), TEXT("CanvasPanel"));
    AddPayload->SetStringField(TEXT("name"), TEXT("RootCanvas"));
    InvokeHandlerWithCapture(TEXT("widget.add"), AddPayload, Capture);

    // Add a text block as child via unified widget.add
    TSharedPtr<FJsonObject> TextPayload = MakeShared<FJsonObject>();
    TextPayload->SetStringField(TEXT("widgetPath"), AssetPath);
    TextPayload->SetStringField(TEXT("type"), TEXT("TextBlock"));
    TextPayload->SetStringField(TEXT("name"), TEXT("MyText"));
    TextPayload->SetStringField(TEXT("parentName"), TEXT("RootCanvas"));
    InvokeHandlerWithCapture(TEXT("widget.add"), TextPayload, Capture);

    // Verify MyText has a GUID
    UWidgetBlueprint* WBP = LoadTestWidgetBlueprint(AssetPath);
    if (TestNotNull(TEXT("widget blueprint loaded"), WBP))
    {
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 6, 0)
        // GUID map is a UE 5.6+ feature; skip the invariant on 5.4/5.5.
        TestTrue(TEXT("MyText has GUID before removal"),
            WBP->WidgetVariableNameToGuidMap.Contains(FName(TEXT("MyText"))));
#endif
    }

    // Remove MyText
    TSharedPtr<FJsonObject> RemovePayload = MakeShared<FJsonObject>();
    RemovePayload->SetStringField(TEXT("widgetPath"), AssetPath);
    RemovePayload->SetStringField(TEXT("slotName"), TEXT("MyText"));
    InvokeHandlerWithCapture(TEXT("widget.remove_widget"), RemovePayload, Capture);
    TestTrue(TEXT("remove succeeded"), Capture.bSuccess);

    // Verify GUID is gone
    WBP = LoadTestWidgetBlueprint(AssetPath);
    if (TestNotNull(TEXT("widget blueprint still loaded"), WBP))
    {
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 6, 0)
        // GUID map is a UE 5.6+ feature; skip the invariant on 5.4/5.5.
        TestFalse(TEXT("MyText GUID removed after widget removal"),
            WBP->WidgetVariableNameToGuidMap.Contains(FName(TEXT("MyText"))));
#endif
    }

    CleanupWidgetAsset(AssetPath);
    return true;
}

// ---- GuidConsistency.RenameWidget ----
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGuidConsistencyRenameWidgetTest,
    "PinWright.widget.GuidConsistency.RenameWidget",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FGuidConsistencyRenameWidgetTest::RunTest(const FString& Parameters)
{
    const FString AssetPath = MakeUniqueWidgetAssetPath(TEXT("WBP_GuidRename"));
    FTestResponseCapture Capture;

    // Create widget blueprint
    TestTrue(TEXT("create handler found"), CreateTestWidgetBlueprint(AssetPath, Capture));
    TestTrue(TEXT("create succeeded"), Capture.bSuccess);

    // Add canvas panel via unified widget.add
    TSharedPtr<FJsonObject> AddPayload = MakeShared<FJsonObject>();
    AddPayload->SetStringField(TEXT("widgetPath"), AssetPath);
    AddPayload->SetStringField(TEXT("type"), TEXT("CanvasPanel"));
    AddPayload->SetStringField(TEXT("name"), TEXT("RootCanvas"));
    InvokeHandlerWithCapture(TEXT("widget.add"), AddPayload, Capture);

    // Add a text block via unified widget.add
    TSharedPtr<FJsonObject> TextPayload = MakeShared<FJsonObject>();
    TextPayload->SetStringField(TEXT("widgetPath"), AssetPath);
    TextPayload->SetStringField(TEXT("type"), TEXT("TextBlock"));
    TextPayload->SetStringField(TEXT("name"), TEXT("OldName"));
    TextPayload->SetStringField(TEXT("parentName"), TEXT("RootCanvas"));
    InvokeHandlerWithCapture(TEXT("widget.add"), TextPayload, Capture);

    // Rename
    TSharedPtr<FJsonObject> RenamePayload = MakeShared<FJsonObject>();
    RenamePayload->SetStringField(TEXT("widgetPath"), AssetPath);
    RenamePayload->SetStringField(TEXT("slotName"), TEXT("OldName"));
    RenamePayload->SetStringField(TEXT("newName"), TEXT("NewName"));
    InvokeHandlerWithCapture(TEXT("widget.rename_widget"), RenamePayload, Capture);
    TestTrue(TEXT("rename succeeded"), Capture.bSuccess);

    // Verify old name gone, new name present
    UWidgetBlueprint* WBP = LoadTestWidgetBlueprint(AssetPath);
    if (TestNotNull(TEXT("widget blueprint loaded"), WBP))
    {
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 6, 0)
        // GUID map is a UE 5.6+ feature; skip the invariant on 5.4/5.5.
        TestFalse(TEXT("OldName GUID removed"),
            WBP->WidgetVariableNameToGuidMap.Contains(FName(TEXT("OldName"))));
        TestTrue(TEXT("NewName GUID present"),
            WBP->WidgetVariableNameToGuidMap.Contains(FName(TEXT("NewName"))));
#endif
    }

    CleanupWidgetAsset(AssetPath);
    return true;
}

// ---- RenameWidget.ValidationAndReadback ----
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWidgetRenameValidationAndReadbackTest,
    "PinWright.widget.RenameWidget.ValidationAndReadback",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWidgetRenameValidationAndReadbackTest::RunTest(const FString& Parameters)
{
    const FString AssetPath = MakeUniqueWidgetAssetPath(TEXT("WBP_RenameValidation"));
    FTestResponseCapture Capture;

    TestTrue(TEXT("create handler found"), CreateTestWidgetBlueprint(AssetPath, Capture));
    TestTrue(TEXT("create succeeded"), Capture.bSuccess);
    if (!Capture.bSuccess)
    {
        CleanupWidgetAsset(AssetPath);
        return true;
    }

    for (const TCHAR* Name : { TEXT("OldName"), TEXT("TakenName") })
    {
        TSharedPtr<FJsonObject> TextPayload = MakeShared<FJsonObject>();
        TextPayload->SetStringField(TEXT("widgetPath"), AssetPath);
        TextPayload->SetStringField(TEXT("type"), TEXT("TextBlock"));
        TextPayload->SetStringField(TEXT("name"), Name);
        TextPayload->SetStringField(TEXT("parentName"), TEXT("RootCanvas"));
        TestTrue(*FString::Printf(TEXT("%s add handler found"), Name),
            InvokeHandlerWithCapture(TEXT("widget.add"), TextPayload, Capture));
        TestTrue(*FString::Printf(TEXT("%s add succeeded"), Name), Capture.bSuccess);
    }

    // Invalid UObject names are rejected before a transaction or tree mutation.
    TSharedPtr<FJsonObject> InvalidPayload = MakeShared<FJsonObject>();
    InvalidPayload->SetStringField(TEXT("widgetPath"), AssetPath);
    InvalidPayload->SetStringField(TEXT("slotName"), TEXT("OldName"));
    InvalidPayload->SetStringField(TEXT("newName"), TEXT("Bad/Name"));
    TestTrue(TEXT("invalid rename handler found"),
        InvokeHandlerWithCapture(TEXT("widget.rename_widget"), InvalidPayload, Capture));
    TestTrue(TEXT("invalid rename responded"), Capture.bWasCalled);
    TestFalse(TEXT("invalid widget name rejected"), Capture.bSuccess);
    TestEqual(TEXT("invalid widget name returns INVALID_ARGUMENT"),
        Capture.ErrorCode, FString(ErrorCodes::ERR_INVALID_ARGUMENT));

    UWidgetBlueprint* WBP = LoadTestWidgetBlueprint(AssetPath);
    UWidget* OldWidget = WBP && WBP->WidgetTree
        ? WBP->WidgetTree->FindWidget(FName(TEXT("OldName")))
        : nullptr;
    if (TestNotNull(TEXT("old widget survives invalid rename"), OldWidget))
    {
        TestNull(TEXT("invalid destination was not created"),
            WBP->WidgetTree->FindWidget(FName(TEXT("Bad/Name"))));

        // A destination collision is rejected without opening a transaction.
        TSharedPtr<FJsonObject> CollisionPayload = MakeShared<FJsonObject>();
        CollisionPayload->SetStringField(TEXT("widgetPath"), AssetPath);
        CollisionPayload->SetStringField(TEXT("slotName"), TEXT("OldName"));
        CollisionPayload->SetStringField(TEXT("newName"), TEXT("TakenName"));
        TestTrue(TEXT("collision rename handler found"),
            InvokeHandlerWithCapture(TEXT("widget.rename_widget"), CollisionPayload, Capture));
        TestTrue(TEXT("collision rename responded"), Capture.bWasCalled);
        TestFalse(TEXT("destination collision rejected"), Capture.bSuccess);
        TestEqual(TEXT("destination collision returns DESTINATION_EXISTS"),
            Capture.ErrorCode, FString(ErrorCodes::ERR_DESTINATION_EXISTS));

        WBP = LoadTestWidgetBlueprint(AssetPath);
        if (TestNotNull(TEXT("widget blueprint survives collision"), WBP))
        {
            TestNotNull(TEXT("old widget survives collision"),
                WBP->WidgetTree->FindWidget(FName(TEXT("OldName"))));
            TestNotNull(TEXT("occupied destination survives collision"),
                WBP->WidgetTree->FindWidget(FName(TEXT("TakenName"))));
        }
    }

    // A successful rename reports the UObject name read back from the tree, not the request.
    TSharedPtr<FJsonObject> RenamePayload = MakeShared<FJsonObject>();
    RenamePayload->SetStringField(TEXT("widgetPath"), AssetPath);
    RenamePayload->SetStringField(TEXT("slotName"), TEXT("OldName"));
    RenamePayload->SetStringField(TEXT("newName"), TEXT("RenamedName"));
    TestTrue(TEXT("valid rename handler found"),
        InvokeHandlerWithCapture(TEXT("widget.rename_widget"), RenamePayload, Capture));
    TestTrue(TEXT("valid rename succeeded"), Capture.bSuccess);

    FString ReturnedName;
    TestTrue(TEXT("successful rename reports newName"),
        Capture.Result.IsValid()
        && Capture.Result->TryGetStringField(TEXT("newName"), ReturnedName));

    WBP = LoadTestWidgetBlueprint(AssetPath);
    if (TestNotNull(TEXT("widget blueprint loaded after rename"), WBP))
    {
        UWidget* RenamedWidget = WBP->WidgetTree->FindWidget(FName(TEXT("RenamedName")));
        TestNotNull(TEXT("renamed widget resolves from tree"), RenamedWidget);
        TestNull(TEXT("old widget identity is gone"),
            WBP->WidgetTree->FindWidget(FName(TEXT("OldName"))));
        if (RenamedWidget && !ReturnedName.IsEmpty())
        {
            TestEqual(TEXT("response newName matches actual widget name"),
                ReturnedName, RenamedWidget->GetName());
        }
    }

    CleanupWidgetAsset(AssetPath);
    return true;
}

#if WITH_DEV_AUTOMATION_TESTS
// ---- RenameWidget.ReadbackFailureRestoresState ----
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWidgetRenameReadbackFailureRestoresStateTest,
    "PinWright.widget.RenameWidget.ReadbackFailureRestoresState",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWidgetRenameReadbackFailureRestoresStateTest::RunTest(const FString& Parameters)
{
    const FString AssetPath = MakeUniqueWidgetAssetPath(TEXT("WBP_RenameReadbackFailure"));
    FTestResponseCapture Capture;

    TestTrue(TEXT("create handler found"), CreateTestWidgetBlueprint(AssetPath, Capture));
    TestTrue(TEXT("create succeeded"), Capture.bSuccess);
    if (!Capture.bSuccess)
    {
        CleanupWidgetAsset(AssetPath);
        return true;
    }

    TSharedPtr<FJsonObject> TextPayload = MakeShared<FJsonObject>();
    TextPayload->SetStringField(TEXT("widgetPath"), AssetPath);
    TextPayload->SetStringField(TEXT("type"), TEXT("TextBlock"));
    TextPayload->SetStringField(TEXT("name"), TEXT("OldName"));
    TextPayload->SetStringField(TEXT("parentName"), TEXT("RootCanvas"));
    TestTrue(TEXT("widget.add handler found"),
        InvokeHandlerWithCapture(TEXT("widget.add"), TextPayload, Capture));
    TestTrue(TEXT("widget.add succeeded"), Capture.bSuccess);

    UWidgetBlueprint* WBP = LoadTestWidgetBlueprint(AssetPath);
    TestNotNull(TEXT("widget blueprint loaded before forced failure"), WBP);
    if (!WBP || !WBP->WidgetTree)
    {
        CleanupWidgetAsset(AssetPath);
        return true;
    }

    UWidget* OriginalWidget = WBP->WidgetTree->FindWidget(FName(TEXT("OldName")));
    TestNotNull(TEXT("original widget resolves before forced failure"), OriginalWidget);
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 6, 0)
    const FGuid* OriginalGuid = WBP->WidgetVariableNameToGuidMap.Find(FName(TEXT("OldName")));
    TestNotNull(TEXT("original widget GUID resolves before forced failure"), OriginalGuid);
    const FGuid OriginalGuidValue = OriginalGuid ? *OriginalGuid : FGuid();
#endif

    TSharedPtr<FJsonObject> RenamePayload = MakeShared<FJsonObject>();
    RenamePayload->SetStringField(TEXT("widgetPath"), AssetPath);
    RenamePayload->SetStringField(TEXT("slotName"), TEXT("OldName"));
    RenamePayload->SetStringField(TEXT("newName"), TEXT("ForcedReadbackName"));
    {
        PinWrightWidgetHierarchyTestHooks::FScopedForcePostRenameReadbackFailure ForceReadback;
        TestTrue(TEXT("forced readback rename handler found"),
            InvokeHandlerWithCapture(TEXT("widget.rename_widget"), RenamePayload, Capture));
    }

    TestTrue(TEXT("forced readback failure responded"), Capture.bWasCalled);
    TestFalse(TEXT("forced readback failure is not success"), Capture.bSuccess);
    TestEqual(TEXT("forced readback failure returns RENAME_FAILED"),
        Capture.ErrorCode, FString(ErrorCodes::ERR_RENAME_FAILED));

    WBP = LoadTestWidgetBlueprint(AssetPath);
    TestNotNull(TEXT("widget blueprint loaded after forced failure"), WBP);
    if (WBP && WBP->WidgetTree)
    {
        UWidget* RestoredWidget = WBP->WidgetTree->FindWidget(FName(TEXT("OldName")));
        TestTrue(TEXT("original widget object is restored"), RestoredWidget == OriginalWidget);
        TestNotNull(TEXT("original widget name resolves after forced failure"), RestoredWidget);
        TestNull(TEXT("forced destination name is absent after rollback"),
            WBP->WidgetTree->FindWidget(FName(TEXT("ForcedReadbackName"))));
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 6, 0)
        const FGuid* RestoredGuid = WBP->WidgetVariableNameToGuidMap.Find(FName(TEXT("OldName")));
        TestTrue(TEXT("original widget GUID is restored"),
            RestoredGuid && *RestoredGuid == OriginalGuidValue);
        TestFalse(TEXT("forced destination GUID is absent after rollback"),
            WBP->WidgetVariableNameToGuidMap.Contains(FName(TEXT("ForcedReadbackName"))));
#endif
    }

    CleanupWidgetAsset(AssetPath);
    return true;
}
#endif

// ---- GuidConsistency.CompileAfterMutations ----
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGuidConsistencyCompileAfterMutationsTest,
    "PinWright.widget.GuidConsistency.CompileAfterMutations",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FGuidConsistencyCompileAfterMutationsTest::RunTest(const FString& Parameters)
{
    const FString AssetPath = MakeUniqueWidgetAssetPath(TEXT("WBP_GuidCompile"));
    FTestResponseCapture Capture;

    // Create widget blueprint
    TestTrue(TEXT("create handler found"), CreateTestWidgetBlueprint(AssetPath, Capture));
    TestTrue(TEXT("create succeeded"), Capture.bSuccess);

    // Add canvas panel as child of the auto-created RootCanvas
    TSharedPtr<FJsonObject> CanvasPayload = MakeShared<FJsonObject>();
    CanvasPayload->SetStringField(TEXT("widgetPath"), AssetPath);
    CanvasPayload->SetStringField(TEXT("type"), TEXT("CanvasPanel"));
    CanvasPayload->SetStringField(TEXT("name"), TEXT("TestCanvas"));
    CanvasPayload->SetStringField(TEXT("parentName"), TEXT("RootCanvas"));
    InvokeHandlerWithCapture(TEXT("widget.add"), CanvasPayload, Capture);

    // Add two text blocks under TestCanvas
    for (const TCHAR* Name : { TEXT("TextA"), TEXT("TextB") })
    {
        TSharedPtr<FJsonObject> TextPayload = MakeShared<FJsonObject>();
        TextPayload->SetStringField(TEXT("widgetPath"), AssetPath);
        TextPayload->SetStringField(TEXT("type"), TEXT("TextBlock"));
        TextPayload->SetStringField(TEXT("name"), Name);
        TextPayload->SetStringField(TEXT("parentName"), TEXT("TestCanvas"));
        InvokeHandlerWithCapture(TEXT("widget.add"), TextPayload, Capture);
    }

    // Remove TextA
    TSharedPtr<FJsonObject> RemovePayload = MakeShared<FJsonObject>();
    RemovePayload->SetStringField(TEXT("widgetPath"), AssetPath);
    RemovePayload->SetStringField(TEXT("slotName"), TEXT("TextA"));
    InvokeHandlerWithCapture(TEXT("widget.remove_widget"), RemovePayload, Capture);

    // Rename TextB -> TextRenamed
    TSharedPtr<FJsonObject> RenamePayload = MakeShared<FJsonObject>();
    RenamePayload->SetStringField(TEXT("widgetPath"), AssetPath);
    RenamePayload->SetStringField(TEXT("slotName"), TEXT("TextB"));
    RenamePayload->SetStringField(TEXT("newName"), TEXT("TextRenamed"));
    InvokeHandlerWithCapture(TEXT("widget.rename_widget"), RenamePayload, Capture);

    // Compile — must not crash in ValidateAndFixUpVariableGuids
    UWidgetBlueprint* WBP = LoadTestWidgetBlueprint(AssetPath);
    if (TestNotNull(TEXT("widget blueprint loaded for compile"), WBP))
    {
        FKismetEditorUtilities::CompileBlueprint(WBP);
        TestTrue(TEXT("compile completed without crash"), true);

        // Verify GUID map consistency after compile.
        // GUID map is a UE 5.6+ feature; skip the invariant on 5.4/5.5 (the compile
        // itself is still exercised on every version above).
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 6, 0)
        TestFalse(TEXT("TextA GUID absent after remove+compile"),
            WBP->WidgetVariableNameToGuidMap.Contains(FName(TEXT("TextA"))));
        TestTrue(TEXT("TextRenamed GUID present after rename+compile"),
            WBP->WidgetVariableNameToGuidMap.Contains(FName(TEXT("TextRenamed"))));
#endif
    }

    CleanupWidgetAsset(AssetPath);
    return true;
}

// ---- RemoveWidget.CascadeBoundEvents (regression for E-widget-remove-widget-cascade-bound-events) ----
// widget.remove_widget must also delete every K2Node_ComponentBoundEvent that
// targeted the removed widget (or any of its descendants, since PanelWidget
// children are cascaded too). Without this, the BP retains dangling
// "does not have a valid matching component" warnings/errors that block compile.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWidgetRemoveWidgetCascadeBoundEventsTest,
    "PinWright.widget.remove_widget.CascadeBoundEvents",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWidgetRemoveWidgetCascadeBoundEventsTest::RunTest(const FString& Parameters)
{
    const FString AssetPath = MakeUniqueWidgetAssetPath(TEXT("WBP_CascadeRemove"));
    FTestResponseCapture Capture;

    TestTrue(TEXT("create handler found"), CreateTestWidgetBlueprint(AssetPath, Capture));
    TestTrue(TEXT("create succeeded"), Capture.bSuccess);

    // Canvas root.
    {
        TSharedPtr<FJsonObject> AddCanvas = MakeShared<FJsonObject>();
        AddCanvas->SetStringField(TEXT("widgetPath"), AssetPath);
        AddCanvas->SetStringField(TEXT("type"), TEXT("CanvasPanel"));
        AddCanvas->SetStringField(TEXT("name"), TEXT("RootCanvas"));
        InvokeHandlerWithCapture(TEXT("widget.add"), AddCanvas, Capture);
    }

    // Button directly under canvas.
    {
        TSharedPtr<FJsonObject> AddBtn = MakeShared<FJsonObject>();
        AddBtn->SetStringField(TEXT("widgetPath"), AssetPath);
        AddBtn->SetStringField(TEXT("type"), TEXT("Button"));
        AddBtn->SetStringField(TEXT("name"), TEXT("BT_Direct"));
        AddBtn->SetStringField(TEXT("parentName"), TEXT("RootCanvas"));
        InvokeHandlerWithCapture(TEXT("widget.add"), AddBtn, Capture);
        TestTrue(TEXT("add BT_Direct succeeded"), Capture.bSuccess);
    }

    // VerticalBox + nested Button to verify descendant cascade.
    {
        TSharedPtr<FJsonObject> AddBox = MakeShared<FJsonObject>();
        AddBox->SetStringField(TEXT("widgetPath"), AssetPath);
        AddBox->SetStringField(TEXT("type"), TEXT("VerticalBox"));
        AddBox->SetStringField(TEXT("name"), TEXT("Container"));
        AddBox->SetStringField(TEXT("parentName"), TEXT("RootCanvas"));
        InvokeHandlerWithCapture(TEXT("widget.add"), AddBox, Capture);

        TSharedPtr<FJsonObject> AddNested = MakeShared<FJsonObject>();
        AddNested->SetStringField(TEXT("widgetPath"), AssetPath);
        AddNested->SetStringField(TEXT("type"), TEXT("Button"));
        AddNested->SetStringField(TEXT("name"), TEXT("BT_Nested"));
        AddNested->SetStringField(TEXT("parentName"), TEXT("Container"));
        InvokeHandlerWithCapture(TEXT("widget.add"), AddNested, Capture);
        TestTrue(TEXT("add BT_Nested succeeded"), Capture.bSuccess);
    }

    // A button we intend to keep — its bound event must survive.
    {
        TSharedPtr<FJsonObject> AddKeep = MakeShared<FJsonObject>();
        AddKeep->SetStringField(TEXT("widgetPath"), AssetPath);
        AddKeep->SetStringField(TEXT("type"), TEXT("Button"));
        AddKeep->SetStringField(TEXT("name"), TEXT("BT_Keep"));
        AddKeep->SetStringField(TEXT("parentName"), TEXT("RootCanvas"));
        InvokeHandlerWithCapture(TEXT("widget.add"), AddKeep, Capture);
        TestTrue(TEXT("add BT_Keep succeeded"), Capture.bSuccess);
    }

    // Bind OnClicked on all three via BPIR.
    auto BindClick = [&](const TCHAR* Name)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), AssetPath);
        Payload->SetStringField(TEXT("mode"), TEXT("append"));
        Payload->SetStringField(TEXT("code"),
            FString::Printf(TEXT("entry widget_event %s.OnClicked() {\n")
                TEXT("    call PrintString(InString: \"%s\")\n")
                TEXT("}"), Name, Name));
        InvokeHandlerWithCapture(TEXT("blueprint.compile_bpir"), Payload, Capture);
        TestTrue(FString::Printf(TEXT("bind OnClicked on %s"), Name), Capture.bSuccess);
    };
    BindClick(TEXT("BT_Direct"));
    BindClick(TEXT("BT_Nested"));
    BindClick(TEXT("BT_Keep"));

    auto CountBoundEvents = [&](TFunction<bool(UK2Node_ComponentBoundEvent*)> Pred)
    {
        UWidgetBlueprint* WBP = LoadTestWidgetBlueprint(AssetPath);
        if (!WBP) return -1;
        int32 Count = 0;
        for (UEdGraph* Graph : WBP->UbergraphPages)
        {
            if (!Graph) continue;
            for (UEdGraphNode* Node : Graph->Nodes)
            {
                if (UK2Node_ComponentBoundEvent* BE = Cast<UK2Node_ComponentBoundEvent>(Node))
                {
                    if (Pred(BE)) ++Count;
                }
            }
        }
        return Count;
    };

    TestEqual(TEXT("3 bound events before removal"),
        CountBoundEvents([](UK2Node_ComponentBoundEvent*){ return true; }), 3);

    // Remove BT_Direct — expect 1 cascaded removal.
    {
        TSharedPtr<FJsonObject> Remove = MakeShared<FJsonObject>();
        Remove->SetStringField(TEXT("widgetPath"), AssetPath);
        Remove->SetStringField(TEXT("slotName"), TEXT("BT_Direct"));
        InvokeHandlerWithCapture(TEXT("widget.remove_widget"), Remove, Capture);
        TestTrue(TEXT("remove BT_Direct succeeded"), Capture.bSuccess);

        double Cascaded = 0.0;
        if (Capture.Result.IsValid())
        {
            Capture.Result->TryGetNumberField(TEXT("cascadedBoundEventsRemoved"), Cascaded);
        }
        TestEqual(TEXT("cascadedBoundEventsRemoved = 1 for BT_Direct"),
            static_cast<int32>(FMath::RoundToInt(Cascaded)), 1);
    }

    TestEqual(TEXT("2 bound events after BT_Direct removal"),
        CountBoundEvents([](UK2Node_ComponentBoundEvent*){ return true; }), 2);
    TestEqual(TEXT("BT_Direct bound event is gone"),
        CountBoundEvents([](UK2Node_ComponentBoundEvent* BE){
            return BE->ComponentPropertyName == FName(TEXT("BT_Direct"));
        }), 0);

    // Remove Container — BT_Nested is a descendant, so its bound event must also cascade.
    {
        TSharedPtr<FJsonObject> Remove = MakeShared<FJsonObject>();
        Remove->SetStringField(TEXT("widgetPath"), AssetPath);
        Remove->SetStringField(TEXT("slotName"), TEXT("Container"));
        InvokeHandlerWithCapture(TEXT("widget.remove_widget"), Remove, Capture);
        TestTrue(TEXT("remove Container succeeded"), Capture.bSuccess);

        double Cascaded = 0.0;
        if (Capture.Result.IsValid())
        {
            Capture.Result->TryGetNumberField(TEXT("cascadedBoundEventsRemoved"), Cascaded);
        }
        TestEqual(TEXT("cascadedBoundEventsRemoved = 1 for Container (BT_Nested cascade)"),
            static_cast<int32>(FMath::RoundToInt(Cascaded)), 1);
    }

    TestEqual(TEXT("BT_Keep bound event survives"),
        CountBoundEvents([](UK2Node_ComponentBoundEvent* BE){
            return BE->ComponentPropertyName == FName(TEXT("BT_Keep"));
        }), 1);
    TestEqual(TEXT("BT_Nested bound event cascade-removed"),
        CountBoundEvents([](UK2Node_ComponentBoundEvent* BE){
            return BE->ComponentPropertyName == FName(TEXT("BT_Nested"));
        }), 0);

    CleanupWidgetAsset(AssetPath);
    return true;
}

// ---- ui.create_hud bare-path resolution (regression for
//      E-create-hud-requires-c-suffix-class-path) ----
// ui.create_hud must resolve a bare Blueprint asset path
// (/Game/.../WBP.WBP, no _C suffix) to the generated UUserWidget class, just
// like every other class-path slot in the API. Previously the handler did a raw
// LoadClass<UUserWidget>(nullptr, *WidgetPath) on the literal string, which
// returns null for a bare path (it resolves the package's UWidgetBlueprint, not
// the generated UClass) and the call failed with CLASS_NOT_FOUND.
//
// This test drives the ui.create_hud handler itself (not just the resolver) so
// it locks down the handler's wiring, not coverage that FResolveUClass* tests in
// TestClassUtils.cpp already provide. ui.create_hud can't add to a viewport
// headlessly — with no PIE it always fails — but the fixed handler now reports
// *which* step failed: CLASS_NOT_FOUND only when widgetPath fails to resolve, and
// NO_VIEWPORT once resolution succeeds but there's no game viewport. So a bare
// path that resolves yields NO_VIEWPORT, while the broken LoadClass version fails
// at resolution with CLASS_NOT_FOUND. Asserting the error is NOT CLASS_NOT_FOUND
// proves the bare path resolved through the handler and would catch a revert of
// the handler back to raw LoadClass on the literal string.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUiCreateHudBarePathResolvesGeneratedClassTest,
    "PinWright.ui.create_hud.BarePathResolvesGeneratedClass",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FUiCreateHudBarePathResolvesGeneratedClassTest::RunTest(const FString& Parameters)
{
    const FString AssetPath = MakeUniqueWidgetAssetPath(TEXT("WBP_CreateHudBarePath"));
    FTestResponseCapture Capture;

    // Create a real WidgetBlueprint (its generated class is a UUserWidget subclass).
    TestTrue(TEXT("create handler found"), CreateTestWidgetBlueprint(AssetPath, Capture));
    TestTrue(TEXT("create succeeded"), Capture.bSuccess);

    // The bare <Path>.<Asset> object path with NO _C suffix — the form a caller
    // naturally derives from widget.describe and the rest of the API.
    const FString BareObjectPath = ToWidgetObjectPath(AssetPath);
    TestFalse(TEXT("path under test has no _C suffix"), BareObjectPath.EndsWith(TEXT("_C")));

    // Drive the real handler with the bare widgetPath.
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("widgetPath"), BareObjectPath);
    TestTrue(TEXT("create_hud handler found"),
        InvokeHandlerWithCapture(TEXT("ui.create_hud"), Payload, Capture));

    // Headless: no PIE/viewport, so the call necessarily fails. The regression
    // assertion is that it does NOT fail at class resolution — a resolved bare
    // path leaves the handler reporting the downstream NO_VIEWPORT/NO_WORLD step,
    // whereas the broken raw-LoadClass version fails earlier with CLASS_NOT_FOUND.
    TestFalse(TEXT("create_hud failed headlessly (no PIE viewport)"), Capture.bSuccess);
    TestNotEqual(TEXT("bare path resolved through handler (not CLASS_NOT_FOUND)"),
        Capture.ErrorCode, FString(TEXT("CLASS_NOT_FOUND")));

    CleanupWidgetAsset(AssetPath);
    return true;
}
