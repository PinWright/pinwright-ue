// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"
#include "Tests/TestUtils.h"
#include "Tests/UI/ActivatableLayerTagFixture.h"
#include "Tests/TestSkipReporting.h"
#include "Tests/Infra/DispatcherTestHelpers.h"
#include "Tests/Infra/ParamSpecTestHelpers.h"
#include "Handlers/UI/ActivatableLayerResolver.h"

#include "Dom/JsonObject.h"
#include "GameplayTagContainer.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"
#include "Widgets/CommonActivatableWidgetContainer.h"

namespace
{
    // Builds a comparable FGameplayTag WITHOUT mutating the project's tag registry: it sets the
    // struct's reflected `TagName` FName directly. FGameplayTag equality (used by the production
    // linear map-walk) and GetTypeHash (used when the fixture map stores the key) both key on
    // TagName, so a tag built this way stores, hashes, and compares exactly like a registered one
    // for the purposes of ResolveStackFromLayersMap. Test-only helper.
    FGameplayTag MakeComparableLayerTag(const TCHAR* TagName)
    {
        FGameplayTag Tag;
        if (FNameProperty* NameProp = CastField<FNameProperty>(
                FGameplayTag::StaticStruct()->FindPropertyByName(TEXT("TagName"))))
        {
            NameProp->SetPropertyValue_InContainer(&Tag, FName(TagName));
        }
        return Tag;
    }
}

// Regression for F-activatable-push-by-layer-tag: the core of the layerTag addressing mode is
// the reflection walk of UPrimaryGameLayout's `Layers` FMapProperty
// (TMap<FGameplayTag, TObjectPtr<UCommonActivatableWidgetContainerBase>>). CommonGame is Lyra-only
// and absent on this host, so the walk is exercised against an in-code fixture exposing that exact
// map shape. It must return the container registered for a present tag, and LAYER_NOT_FOUND for an
// absent one. Reverting the walk (wrong property name, wrong key comparison, or wrong value read)
// fails these assertions.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUiActivatableLayerTagResolvesFromMapTest,
    "PinWright.ui.activatable.LayerTagResolvesFromLayersMap",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FUiActivatableLayerTagResolvesFromMapTest::RunTest(const FString& Parameters)
{
    UActivatableLayerTagFixture* Fixture = NewObject<UActivatableLayerTagFixture>(GetTransientPackage());
    if (!TestNotNull(TEXT("layout fixture created"), Fixture))
    {
        return true;
    }

    // Concrete UCommonActivatableWidgetContainerBase subclasses (the base is abstract) stored as
    // the map values, exactly as UPrimaryGameLayout::RegisterLayer records real layer containers.
    UCommonActivatableWidgetStack* MenuStack = NewObject<UCommonActivatableWidgetStack>(GetTransientPackage());
    UCommonActivatableWidgetStack* ModalStack = NewObject<UCommonActivatableWidgetStack>(GetTransientPackage());
    if (!TestNotNull(TEXT("menu stack created"), MenuStack) || !TestNotNull(TEXT("modal stack created"), ModalStack))
    {
        return true;
    }

    const FGameplayTag MenuTag = MakeComparableLayerTag(TEXT("UI.Layer.Menu"));
    const FGameplayTag ModalTag = MakeComparableLayerTag(TEXT("UI.Layer.Modal"));
    const FGameplayTag MissingTag = MakeComparableLayerTag(TEXT("UI.Layer.Game"));
    TestTrue(TEXT("menu tag is valid"), MenuTag.IsValid());
    TestTrue(TEXT("menu and modal tags are distinct"), MenuTag != ModalTag);

    Fixture->Layers.Add(MenuTag, MenuStack);
    Fixture->Layers.Add(ModalTag, ModalStack);

    // Present tag -> its exact registered container instance.
    {
        FString Code, Msg;
        UCommonActivatableWidgetContainerBase* Resolved =
            PinWrightUi::ResolveStackFromLayersMap(Fixture, MenuTag, Code, Msg);
        TestTrue(TEXT("UI.Layer.Menu resolves to the MenuStack instance"),
            Resolved == static_cast<UCommonActivatableWidgetContainerBase*>(MenuStack));
    }
    {
        FString Code, Msg;
        UCommonActivatableWidgetContainerBase* Resolved =
            PinWrightUi::ResolveStackFromLayersMap(Fixture, ModalTag, Code, Msg);
        TestTrue(TEXT("UI.Layer.Modal resolves to the ModalStack instance"),
            Resolved == static_cast<UCommonActivatableWidgetContainerBase*>(ModalStack));
    }

    // Absent tag -> null + LAYER_NOT_FOUND (not a crash, not the wrong container).
    {
        FString Code, Msg;
        UCommonActivatableWidgetContainerBase* Resolved =
            PinWrightUi::ResolveStackFromLayersMap(Fixture, MissingTag, Code, Msg);
        TestNull(TEXT("an unregistered layer tag resolves to null"), Resolved);
        TestEqual(TEXT("absent layer tag yields LAYER_NOT_FOUND"), Code, FString(TEXT("LAYER_NOT_FOUND")));
    }

    return true;
}

// Regression for F-activatable-push-by-layer-tag: on a host without CommonGame (the plugin
// deliberately does not link/enable it, but Lyra-fork hosts ship it — the test skips there),
// the layerTag branch must degrade gracefully to a
// clean LAYER_HOST_UNAVAILABLE error rather than crash or fall through to a host-required error.
// This drives the real ui.list_stack_widgets handler through the new ResolveTargetStack dispatcher.
// Before the fix, host was a required param, so a layerTag-only call would fail with a missing-param
// error instead of LAYER_HOST_UNAVAILABLE — reverting the branch fails this test.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUiActivatableLayerTagHostUnavailableTest,
    "PinWright.ui.activatable.LayerTagHostUnavailableWithoutCommonGame",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FUiActivatableLayerTagHostUnavailableTest::RunTest(const FString& Parameters)
{
    // Host gate: the LAYER_HOST_UNAVAILABLE branch is only reachable where CommonGame's
    // UPrimaryGameLayout genuinely is absent. On a Lyra-fork host the class resolves, so the
    // no-CommonGame path under test cannot fire — pass early with the audit-greppable note
    // (never load Lyra content to make the assertion pass).
    UClass* LayoutClass = FindObject<UClass>(nullptr, TEXT("/Script/CommonGame.PrimaryGameLayout"));
    if (LayoutClass)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("optional-plugin-enabled"),
            TEXT("FIXTURE-SKIP: CommonGame present on this host; test validates the no-CommonGame path."));
        return true;
    }

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("layerTag"), TEXT("UI.Layer.Menu"));

    FTestResponseCapture Capture;
    TestTrue(TEXT("ui.list_stack_widgets handler found"),
        InvokeHandlerWithCapture(TEXT("ui.list_stack_widgets"), Payload, Capture));
    TestTrue(TEXT("response was sent"), Capture.bWasCalled);
    TestFalse(TEXT("layerTag on a non-CommonGame host must not succeed"), Capture.bSuccess);
    TestEqual(TEXT("layerTag without CommonGame yields LAYER_HOST_UNAVAILABLE"),
        Capture.ErrorCode, FString(TEXT("LAYER_HOST_UNAVAILABLE")));

    return true;
}

// Regression for B-layertag-declared-integer-blocks-tag-addressing: layerTag carries a gameplay
// tag, so it must be declared `string`. It was flipped to `integer` by a discrete-index name-token
// sweep, after which the dispatcher's declared-type gate refused every tag value with
// PARAM_TYPE_MISMATCH before the handler ran, and the one value that parses ("123") died later
// with the unrelated LAYER_TAG_INVALID. One macro (ACTIVATABLE_TARGET_PARAMS) declares the
// parameter for all four verbs, so all four are asserted here.
// Counterfactual: restore "integer" in ACTIVATABLE_TARGET_PARAMS and each declared type reads
// "integer", failing every TestEqual below.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUiActivatableLayerTagDeclaredAsStringTest,
    "PinWright.ui.activatable.LayerTagDeclaredAsString",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FUiActivatableLayerTagDeclaredAsStringTest::RunTest(const FString& Parameters)
{
    const TCHAR* Methods[] = {
        TEXT("ui.activatable_push"),
        TEXT("ui.activatable_pop"),
        TEXT("ui.list_stack_widgets"),
        TEXT("ui.get_active_widget")
    };

    for (const TCHAR* Method : Methods)
    {
        const FParamSpec* Spec = ParamSpecTestHelpers::FindParamSpec(Method, TEXT("layerTag"));
        if (!TestTrue(FString::Printf(TEXT("%s declares a layerTag param"), Method), Spec != nullptr))
        {
            continue;
        }
        TestEqual(FString::Printf(TEXT("%s.layerTag is declared string"), Method),
            Spec->Type, FString(TEXT("string")));
    }

    return true;
}

// The gate-level half of the same regression, and the reason the sibling tests above could not
// catch it: Tests/TestUtils.h's InvokeHandlerWithCapture calls the handler function directly and
// never runs FRpcDispatcher::ValidateHandlerParams, so a handler that reads layerTag as a string
// stays green against a declaration the real dispatcher refuses. This routes a real gameplay-tag
// payload through a real FRpcDispatcher, declared-type gate included.
// Counterfactual: restore "integer" and the gate answers PARAM_TYPE_MISMATCH instead of letting
// the payload reach the body, failing both error-code assertions.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUiActivatableLayerTagPassesDeclaredTypeGateTest,
    "PinWright.ui.activatable.LayerTagPassesDeclaredTypeGate",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FUiActivatableLayerTagPassesDeclaredTypeGateTest::RunTest(const FString& Parameters)
{
    DispatcherTestHelpers::FSinkPtr Sink;
    FRpcDispatcher Dispatcher;
    DispatcherTestHelpers::MakeDispatcher(Sink, Dispatcher);

    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("layerTag"), TEXT("UI.Layer.Menu"));
    // widgetClass is required and resolved before any stack lookup, so an unloadable class path
    // ends the call in the handler body with a host-independent CLASS_NOT_FOUND: proving the
    // payload cleared the gate needs no PIE, no CommonGame and no real widget asset. Same probe
    // path as PinWright.ui.activatable.PushRejectsBadClass.
    Params->SetStringField(TEXT("widgetClass"), TEXT("/Game/_Test/NoSuchClass"));

    bool bSuccess = false;
    FString ErrorCode;
    DispatcherTestHelpers::Dispatch(Dispatcher, Sink, TEXT("ui.activatable_push"),
        TEXT("req-layertag-declared-type"), Params, bSuccess, ErrorCode);

    TestTrue(TEXT("the dispatcher answered"), Sink->bWasCalled);
    TestNotEqual(TEXT("a gameplay-tag layerTag is not refused by the declared-type gate"),
        ErrorCode, FString(TEXT("PARAM_TYPE_MISMATCH")));
    TestEqual(TEXT("the payload reaches the handler body, which reports CLASS_NOT_FOUND"),
        ErrorCode, FString(TEXT("CLASS_NOT_FOUND")));
    TestFalse(TEXT("the unloadable-class call does not fake-succeed"), bSuccess);

    return true;
}
