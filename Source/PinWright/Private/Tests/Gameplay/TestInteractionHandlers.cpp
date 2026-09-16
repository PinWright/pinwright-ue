// Copyright (c) 2026 Alexander Penkin. MIT License.

// Unit tests for Interaction domain handlers (InteractionHandler.cpp)
// Covers all 23 REGISTER_RPC_HANDLER entries in the Interaction/ folder.
#include "Misc/AutomationTest.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/HandlerRegistration.h"
#include "Handlers/ParamSpec.h"
#include "Dom/JsonObject.h"
#include "Tests/TestUtils.h"
#include "Tests/Bpir/CompilerTestUtils.h"
#include "PinWrightHelpers.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Blueprint/UserWidget.h"
#include "Engine/DataTable.h"
#include "EdGraph/EdGraph.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "UObject/UnrealType.h"


// ============================================================================
// interaction.create_interaction_component
// REQ: blueprintPath
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FInteractionCreateComponentValidParamsTest,
    "PinWright.interaction.create_interaction_component.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FInteractionCreateComponentValidParamsTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("blueprintPath"), TEXT("/Game/Blueprints/BP_TestActor"));
    Payload->SetStringField(TEXT("componentName"), TEXT("InteractionSphere"));
    Payload->SetNumberField(TEXT("traceDistance"), 200.0);
    TestTrue(TEXT("Handler registered"), InvokeHandler(TEXT("interaction.create_interaction_component"), Payload));
    return true;
}

// ============================================================================
// interaction.configure_interaction_trace
// REQ: blueprintPath
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FInteractionConfigureTraceValidParamsTest,
    "PinWright.interaction.configure_interaction_trace.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FInteractionConfigureTraceValidParamsTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("blueprintPath"), TEXT("/Game/Blueprints/BP_TestActor"));
    Payload->SetNumberField(TEXT("traceDistance"), 350.0);
    Payload->SetStringField(TEXT("traceChannel"), TEXT("Visibility"));
    TestTrue(TEXT("Handler registered"), InvokeHandler(TEXT("interaction.configure_interaction_trace"), Payload));
    return true;
}

// ============================================================================
// interaction.configure_interaction_widget
// REQ: blueprintPath
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FInteractionConfigureWidgetValidParamsTest,
    "PinWright.interaction.configure_interaction_widget.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FInteractionConfigureWidgetValidParamsTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("blueprintPath"), TEXT("/Game/Blueprints/BP_TestActor"));
    Payload->SetStringField(TEXT("widgetClass"), TEXT("WBP_InteractPrompt"));
    Payload->SetStringField(TEXT("widgetText"), TEXT("Press E to interact"));
    TestTrue(TEXT("Handler registered"), InvokeHandler(TEXT("interaction.configure_interaction_widget"), Payload));
    return true;
}

// ============================================================================
// interaction.add_interaction_events
// REQ: blueprintPath
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FInteractionAddEventsValidParamsTest,
    "PinWright.interaction.add_interaction_events.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FInteractionAddEventsValidParamsTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("blueprintPath"), TEXT("/Game/Blueprints/BP_TestActor"));
    TestTrue(TEXT("Handler registered"), InvokeHandler(TEXT("interaction.add_interaction_events"), Payload));
    return true;
}

// ============================================================================
// interaction.configure_door_properties
// REQ: doorPath
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FInteractionConfigureDoorValidParamsTest,
    "PinWright.interaction.configure_door_properties.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FInteractionConfigureDoorValidParamsTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("doorPath"), TEXT("/Game/Blueprints/Doors/BP_Door_Test"));
    Payload->SetBoolField(TEXT("isLocked"), true);
    Payload->SetNumberField(TEXT("openAngle"), 90.0);
    TestTrue(TEXT("Handler registered"), InvokeHandler(TEXT("interaction.configure_door_properties"), Payload));
    return true;
}
// ============================================================================
// interaction.configure_switch_properties
// REQ: switchPath
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FInteractionConfigureSwitchValidParamsTest,
    "PinWright.interaction.configure_switch_properties.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FInteractionConfigureSwitchValidParamsTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("switchPath"), TEXT("/Game/Blueprints/Switches/BP_Switch_Test"));
    Payload->SetBoolField(TEXT("isToggle"), true);
    TestTrue(TEXT("Handler registered"), InvokeHandler(TEXT("interaction.configure_switch_properties"), Payload));
    return true;
}
// ============================================================================
// interaction.configure_chest_properties
// REQ: chestPath
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FInteractionConfigureChestValidParamsTest,
    "PinWright.interaction.configure_chest_properties.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FInteractionConfigureChestValidParamsTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("chestPath"), TEXT("/Game/Blueprints/Chests/BP_Chest_Test"));
    Payload->SetBoolField(TEXT("isLocked"), true);
    Payload->SetStringField(TEXT("requiredKey"), TEXT("GoldenKey"));
    TestTrue(TEXT("Handler registered"), InvokeHandler(TEXT("interaction.configure_chest_properties"), Payload));
    return true;
}
// ============================================================================
// interaction.add_destruction_component
// REQ: blueprintPath
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FInteractionAddDestructionValidParamsTest,
    "PinWright.interaction.add_destruction_component.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FInteractionAddDestructionValidParamsTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("blueprintPath"), TEXT("/Game/Blueprints/BP_DestructibleProp"));
    Payload->SetNumberField(TEXT("maxHealth"), 150.0);
    Payload->SetBoolField(TEXT("destroyOnDeath"), true);
    TestTrue(TEXT("Handler registered"), InvokeHandler(TEXT("interaction.add_destruction_component"), Payload));
    return true;
}
// ============================================================================
// interaction.get_interaction_info
// All params optional — test both empty payload and populated payload
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FInteractionGetInfoEmptyPayloadTest,
    "PinWright.interaction.get_interaction_info.EmptyPayloadNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FInteractionGetInfoEmptyPayloadTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    TestTrue(TEXT("Handler registered"), InvokeHandler(TEXT("interaction.get_interaction_info"), Payload));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FInteractionGetInfoWithParamsTest,
    "PinWright.interaction.get_interaction_info.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FInteractionGetInfoWithParamsTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("blueprintPath"), TEXT("/Game/Blueprints/BP_TestActor"));
    Payload->SetStringField(TEXT("actorName"), TEXT("BP_TestActor_0"));
    TestTrue(TEXT("Handler registered"), InvokeHandler(TEXT("interaction.get_interaction_info"), Payload));
    return true;
}

// ----------------------------------------------------------------------------
// Shared assertions for the configure_* CDO-writeback regression tests below.
// Each folds the FindFProperty + CastField + Test*/AddError-on-missing idiom into
// one call so the parallel chest / widget tests stay readable and report a missing
// property identically. Uniquely named (Interaction-prefixed) to avoid Unity ODR
// collisions with same-named statics in sibling test translation units.
// ----------------------------------------------------------------------------

// Fetches the compiled GeneratedClass + CDO after a handler ran, failing the test
// (never skipping) when either is absent. Returns false if the fixture didn't compile.
static bool GetInteractionCompiledCdo(FAutomationTestBase& Test, UBlueprint* BP,
    UClass*& OutGenClass, UObject*& OutCDO)
{
    OutGenClass = BP->GeneratedClass.Get();
    if (!Test.TestNotNull(TEXT("GeneratedClass exists after compile"), OutGenClass))
    {
        return false;
    }
    OutCDO = OutGenClass->GetDefaultObject();
    return Test.TestNotNull(TEXT("compiled CDO exists"), OutCDO);
}

static void AssertInteractionCdoBool(FAutomationTestBase& Test, UClass* GenClass, UObject* CDO,
    const TCHAR* PropertyName, bool bExpected, const TCHAR* Description)
{
    if (FBoolProperty* Prop = CastField<FBoolProperty>(FindFProperty<FProperty>(GenClass, PropertyName)))
    {
        Test.TestTrue(Description, Prop->GetPropertyValue_InContainer(CDO) == bExpected);
    }
    else
    {
        Test.AddError(FString::Printf(TEXT("%s property missing from the compiled CDO"), PropertyName));
    }
}

static void AssertInteractionCdoFloat(FAutomationTestBase& Test, UClass* GenClass, UObject* CDO,
    const TCHAR* PropertyName, double Expected, const TCHAR* Description)
{
    if (FNumericProperty* Prop = CastField<FNumericProperty>(FindFProperty<FProperty>(GenClass, PropertyName)))
    {
        Test.TestEqual(Description, Prop->GetFloatingPointPropertyValue(Prop->ContainerPtrToValuePtr<void>(CDO)), Expected);
    }
    else
    {
        Test.AddError(FString::Printf(TEXT("%s property missing from the compiled CDO"), PropertyName));
    }
}

static void AssertInteractionCdoString(FAutomationTestBase& Test, UClass* GenClass, UObject* CDO,
    const TCHAR* PropertyName, const FString& Expected, const TCHAR* Description)
{
    if (FStrProperty* Prop = CastField<FStrProperty>(FindFProperty<FProperty>(GenClass, PropertyName)))
    {
        Test.TestEqual(Description, Prop->GetPropertyValue_InContainer(CDO), Expected);
    }
    else
    {
        Test.AddError(FString::Printf(TEXT("%s property missing from the compiled CDO"), PropertyName));
    }
}

// ============================================================================
// Regression (B-configure-chest-properties-no-cdo-writeback):
// interaction.configure_chest_properties historically scaffolded the chest's
// member variables (bIsLocked / LidOpenAngle / OpenTime / LootTable) with NO
// default, then echoed the input args alongside `configured:true` — so after the
// compile every configured value read back as the type's zero default and the
// response was a silent false-success. The fix bakes each value onto the
// variable's FBPVariableDescription.DefaultValue before the single compile (the
// durable engine-native AddMemberVariable(BP, Name, Type, DefaultValue) path), so
// it lands on the CDO and survives recompiles, and echoes the values read back off
// the CDO. This drives the real registered handler on a fresh in-code Actor
// Blueprint (no external asset) and asserts the compiled CDO actually holds the
// requested values. Reverting to the no-default add makes the CDO read
// false/0/0 and fails the core assertions.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FInteractionConfigureChestDefaultsLandOnCdoTest,
    "PinWright.interaction.configure_chest_properties.DefaultsLandOnCdo",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FInteractionConfigureChestDefaultsLandOnCdoTest::RunTest(const FString& Parameters)
{
    // Build the fixture in-code (a fresh transient Actor Blueprint) — a missing
    // fixture is a failure, never a skip.
    UBlueprint* BP = CompilerTestUtils::CreateTransientTestBP(TEXT("ChestCdoWriteback"));
    if (!TestNotNull(TEXT("chest Blueprint fixture created"), BP))
    {
        return false;
    }

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("chestPath"), BP->GetPathName());
    Payload->SetBoolField(TEXT("locked"), true);
    Payload->SetNumberField(TEXT("openAngle"), 95.0);
    Payload->SetNumberField(TEXT("openTime"), 0.5);

    FTestResponseCapture Capture;
    TestTrue(TEXT("configure_chest_properties handler registered"),
        InvokeHandlerWithCapture(TEXT("interaction.configure_chest_properties"), Payload, Capture));
    TestTrue(TEXT("configure_chest_properties succeeded"), Capture.bSuccess);

    // Core assertion: the requested values actually landed on the compiled CDO.
    // Pre-fix these read back false/0/0 because the variables carried no default.
    UClass* GenClass = nullptr;
    UObject* CDO = nullptr;
    if (GetInteractionCompiledCdo(*this, BP, GenClass, CDO))
    {
        AssertInteractionCdoBool(*this, GenClass, CDO, TEXT("bIsLocked"), true,
            TEXT("CDO bIsLocked is true, not the false zero default"));
        AssertInteractionCdoFloat(*this, GenClass, CDO, TEXT("LidOpenAngle"), 95.0,
            TEXT("CDO LidOpenAngle is 95, not the zero default"));
        AssertInteractionCdoFloat(*this, GenClass, CDO, TEXT("OpenTime"), 0.5,
            TEXT("CDO OpenTime is 0.5, not the zero default"));

        // Reviewer-flagged regression (mirrors the InteractionWidgetClass meta-class fix):
        // the LootTable member must compile as a soft-object reference constrained to
        // UDataTable — the loot-table asset type — NOT silently degraded to a bare UObject
        // meta-class. The handler builds SoftObjectType with PC_SoftObject; a null
        // PinSubCategoryObject makes the Blueprint compiler default the PropertyClass to
        // UObject with NO warning (KismetCompilerMisc's PC_Object / PC_Interface /
        // PC_SoftObject branch), letting LootTable hold any asset. The fix sets
        // PinSubCategoryObject = UDataTable::StaticClass(). Reverting to the null-meta-class
        // pin type degrades PropertyClass to UObject and fails the equality assertion.
        FProperty* LootRawProp = FindFProperty<FProperty>(GenClass, TEXT("LootTable"));
        if (FSoftObjectProperty* LootProp = CastField<FSoftObjectProperty>(LootRawProp))
        {
            if (TestNotNull(TEXT("LootTable carries a property (meta) class"),
                    LootProp->PropertyClass.Get()))
            {
                // Equality to UDataTable also rules out the degraded bare-UObject meta-class,
                // since UDataTable::StaticClass() != UObject::StaticClass().
                TestTrue(TEXT("LootTable meta-class is UDataTable (well-formed loot-table ref, not the degraded UObject)"),
                    LootProp->PropertyClass == UDataTable::StaticClass());
            }
        }
        else
        {
            AddError(TEXT("LootTable is not a soft-object property on the compiled class"));
        }
    }

    // The response must echo the values that landed (read off the CDO), not a bare
    // mirror of the request — so a caller can trust `configured:true` without a
    // property.get round-trip.
    if (TestTrue(TEXT("response object present"), Capture.Result.IsValid()))
    {
        bool bEchoedLocked = false;
        TestTrue(TEXT("response echoes locked"),
            Capture.Result->TryGetBoolField(TEXT("locked"), bEchoedLocked));
        TestTrue(TEXT("echoed locked is true"), bEchoedLocked);

        double EchoedAngle = 0.0;
        TestTrue(TEXT("response echoes openAngle"),
            Capture.Result->TryGetNumberField(TEXT("openAngle"), EchoedAngle));
        TestEqual(TEXT("echoed openAngle is 95"), EchoedAngle, 95.0);

        double EchoedTime = 0.0;
        TestTrue(TEXT("response echoes openTime"),
            Capture.Result->TryGetNumberField(TEXT("openTime"), EchoedTime));
        TestEqual(TEXT("echoed openTime is 0.5"), EchoedTime, 0.5);
    }

    return true;
}

// ============================================================================
// Regression (B-configure-chest-properties-no-cdo-writeback, #3 sibling verbs):
// interaction.configure_interaction_widget carried the identical no-CDO-writeback
// defect — it added bShowOnHover / bShowPromptText / PromptTextFormat with no
// default and echoed the inputs, so after the compile the CDO held false/false/"".
// The fix bakes each value onto the variable's FBPVariableDescription.DefaultValue
// before the single compile. This drives the real handler on a fresh in-code Actor
// Blueprint and asserts the compiled CDO holds the requested values. (Correctly
// populating InteractionWidgetClass is tracked as a separate ticket, so it is not
// asserted here.) Reverting to the no-default add makes the CDO read false/"" and
// fails the core assertions.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FInteractionConfigureWidgetDefaultsLandOnCdoTest,
    "PinWright.interaction.configure_interaction_widget.DefaultsLandOnCdo",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FInteractionConfigureWidgetDefaultsLandOnCdoTest::RunTest(const FString& Parameters)
{
    UBlueprint* BP = CompilerTestUtils::CreateTransientTestBP(TEXT("WidgetCdoWriteback"));
    if (!TestNotNull(TEXT("widget Blueprint fixture created"), BP))
    {
        return false;
    }

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("blueprintPath"), BP->GetPathName());
    Payload->SetBoolField(TEXT("showOnHover"), true);
    Payload->SetBoolField(TEXT("showPromptText"), false);
    Payload->SetStringField(TEXT("promptTextFormat"), TEXT("Press E to open"));

    FTestResponseCapture Capture;
    TestTrue(TEXT("configure_interaction_widget handler registered"),
        InvokeHandlerWithCapture(TEXT("interaction.configure_interaction_widget"), Payload, Capture));
    TestTrue(TEXT("configure_interaction_widget succeeded"), Capture.bSuccess);

    // Core assertion: the requested values actually landed on the compiled CDO.
    // Pre-fix these read back false/"" because the variables carried no default.
    UClass* GenClass = nullptr;
    UObject* CDO = nullptr;
    if (GetInteractionCompiledCdo(*this, BP, GenClass, CDO))
    {
        AssertInteractionCdoBool(*this, GenClass, CDO, TEXT("bShowOnHover"), true,
            TEXT("CDO bShowOnHover is true, not the false zero default"));
        AssertInteractionCdoString(*this, GenClass, CDO, TEXT("PromptTextFormat"),
            FString(TEXT("Press E to open")),
            TEXT("CDO PromptTextFormat holds the requested string, not empty"));
    }

    // The response echoes the values that landed (read off the CDO), not a bare mirror.
    if (TestTrue(TEXT("response object present"), Capture.Result.IsValid()))
    {
        bool bEchoedHover = false;
        TestTrue(TEXT("response echoes showOnHover"),
            Capture.Result->TryGetBoolField(TEXT("showOnHover"), bEchoedHover));
        TestTrue(TEXT("echoed showOnHover is true"), bEchoedHover);

        FString EchoedFormat;
        TestTrue(TEXT("response echoes promptTextFormat"),
            Capture.Result->TryGetStringField(TEXT("promptTextFormat"), EchoedFormat));
        TestEqual(TEXT("echoed promptTextFormat matches request"),
            EchoedFormat, FString(TEXT("Press E to open")));
    }

    return true;
}

// ============================================================================
// Regression (B-configure-interaction-widget-invalid-widget-class):
// interaction.configure_interaction_widget created the InteractionWidgetClass member
// as a PC_SoftClass pin type with NO PinSubCategoryObject (a null meta-class). A class /
// soft-class property with a null meta-class is malformed: on the compile the Blueprint
// compiler substitutes UObject for the meta-class and logs "Invalid property
// 'InteractionWidgetClass' class, replaced with Object. Please fix or remove." — the slot
// degrades to a bare UObject reference that can no longer hold the intended UUserWidget
// subclass. The fix constrains the pin type to UUserWidget (PinSubCategoryObject =
// UUserWidget::StaticClass()). This drives the real registered handler on a fresh in-code
// Actor Blueprint and asserts the compiled property is a soft-class property whose
// meta-class is UUserWidget, not the degraded UObject. Reverting to the null-meta-class
// pin type makes the compiler downgrade the meta-class to UObject and fails the assertion.
// (Distinct from DefaultsLandOnCdo above, which asserts the bShowOnHover/PromptTextFormat
// VALUES land — this asserts the InteractionWidgetClass variable TYPE is well-formed.)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FInteractionConfigureWidgetClassMetaClassResolvedTest,
    "PinWright.interaction.configure_interaction_widget.WidgetClassMetaClassResolved",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FInteractionConfigureWidgetClassMetaClassResolvedTest::RunTest(const FString& Parameters)
{
    // Build the fixture in-code (a fresh transient Actor Blueprint) — a missing fixture is a
    // failure, never a skip.
    UBlueprint* BP = CompilerTestUtils::CreateTransientTestBP(TEXT("WidgetClassMetaClass"));
    if (!TestNotNull(TEXT("widget Blueprint fixture created"), BP))
    {
        return false;
    }

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("blueprintPath"), BP->GetPathName());

    FTestResponseCapture Capture;
    TestTrue(TEXT("configure_interaction_widget handler registered"),
        InvokeHandlerWithCapture(TEXT("interaction.configure_interaction_widget"), Payload, Capture));
    TestTrue(TEXT("configure_interaction_widget succeeded"), Capture.bSuccess);

    // Core assertion: the InteractionWidgetClass property compiled as a soft-class reference
    // constrained to UUserWidget — NOT downgraded to a bare UObject meta-class (the exact
    // "Invalid property ... replaced with Object" symptom the malformed null-meta-class pin
    // type produces).
    UClass* GenClass = nullptr;
    UObject* CDO = nullptr;
    if (GetInteractionCompiledCdo(*this, BP, GenClass, CDO))
    {
        FProperty* RawProp = FindFProperty<FProperty>(GenClass, TEXT("InteractionWidgetClass"));
        if (FSoftClassProperty* SoftClassProp = CastField<FSoftClassProperty>(RawProp))
        {
            if (TestNotNull(TEXT("InteractionWidgetClass carries a meta-class"),
                    SoftClassProp->MetaClass.Get()))
            {
                // Equality to UUserWidget also rules out the degraded bare-UObject meta-class
                // (the "Invalid property ... replaced with Object" symptom) — a single check,
                // since UUserWidget::StaticClass() != UObject::StaticClass().
                TestTrue(TEXT("InteractionWidgetClass meta-class is UUserWidget (well-formed widget-class ref, not the degraded UObject)"),
                    SoftClassProp->MetaClass == UUserWidget::StaticClass());
            }
        }
        else
        {
            AddError(TEXT("InteractionWidgetClass is not a soft-class property on the compiled class"));
        }
    }

    return true;
}

// ============================================================================
// Regression (B-add-interaction-events-delegates-no-signature):
// interaction.add_interaction_events historically added the four OnInteraction* events
// as PC_MCDelegate member variables with NO delegate signature graph, so each
// FMulticastDelegateProperty compiled with a null SignatureFunction — the engine warned
// "No SignatureFunction in MulticastDelegateProperty '<name>'" on the next compile and
// consumers could not bind the events (the handler still reported eventCount:4 success).
// The fix routes each event through the shared AddDispatcherWithSignatureGraph recipe (the
// same one blueprint.add_dispatcher uses), which registers a <Name> signature graph whose
// generated <Name>__DelegateSignature UFunction backs the property. This drives the real
// registered handler on a fresh in-code Actor Blueprint, compiles it (mirroring the repro's
// next compile), and asserts every event's compiled multicast-delegate property has a
// non-null SignatureFunction plus a matching __DelegateSignature UFunction. Reverting to the
// signature-less AddMemberVariable makes SignatureFunction null and fails these assertions.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FInteractionAddEventsSignatureFunctionTest,
    "PinWright.interaction.add_interaction_events.DispatchersHaveSignatureFunction",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FInteractionAddEventsSignatureFunctionTest::RunTest(const FString& Parameters)
{
    // Build the fixture in-code (a fresh transient Actor Blueprint) — a missing fixture
    // is a failure, never a skip.
    UBlueprint* BP = CompilerTestUtils::CreateTransientTestBP(TEXT("AddInteractionEventsSig"));
    if (!TestNotNull(TEXT("interaction Blueprint fixture created"), BP))
    {
        return false;
    }

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("blueprintPath"), BP->GetPathName());

    FTestResponseCapture Capture;
    TestTrue(TEXT("add_interaction_events handler registered"),
        InvokeHandlerWithCapture(TEXT("interaction.add_interaction_events"), Payload, Capture));
    TestTrue(TEXT("add_interaction_events succeeded"), Capture.bSuccess);

    static const TCHAR* const EventNames[] = {
        TEXT("OnInteractionStart"), TEXT("OnInteractionEnd"),
        TEXT("OnInteractableFound"), TEXT("OnInteractableLost")
    };

    // Each event must be registered as a delegate signature graph (named <Event>) — the
    // exact step the pre-fix handler omitted.
    for (const TCHAR* EventName : EventNames)
    {
        bool bFoundGraph = false;
        for (UEdGraph* Graph : BP->DelegateSignatureGraphs)
        {
            if (Graph && Graph->GetName() == EventName)
            {
                bFoundGraph = true;
                break;
            }
        }
        TestTrue(FString::Printf(TEXT("DelegateSignatureGraphs contains %s"), EventName), bFoundGraph);
    }

    // Compile the Blueprint (this is where the missing SignatureFunction surfaced in the
    // repro) and assert every event's multicast-delegate property now has a backing
    // signature UFunction.
    FKismetEditorUtilities::CompileBlueprint(BP);
    UClass* GenClass = BP->GeneratedClass.Get();
    if (TestNotNull(TEXT("GeneratedClass exists after compile"), GenClass))
    {
        for (const TCHAR* EventName : EventNames)
        {
            FMulticastDelegateProperty* DelegateProp =
                FindFProperty<FMulticastDelegateProperty>(GenClass, EventName);
            if (TestNotNull(FString::Printf(TEXT("GeneratedClass has FMulticastDelegateProperty %s"), EventName), DelegateProp))
            {
                TestNotNull(FString::Printf(TEXT("%s has a backing SignatureFunction"), EventName),
                    DelegateProp->SignatureFunction.Get());
            }

            UFunction* SigFunc = GenClass->FindFunctionByName(
                FName(*(FString(EventName) + TEXT("__DelegateSignature"))));
            TestNotNull(FString::Printf(TEXT("GeneratedClass has %s__DelegateSignature"), EventName), SigFunc);
        }
    }

    return true;
}

// ============================================================================
// Regression (E-get-interaction-info-blueprint-bare):
// interaction.get_interaction_info's blueprintPath branch historically emitted only
// {blueprintPath, blueprintName} — none of the sphere/trace/widget/events/chest state
// the sibling create_*/configure_* verbs wrote to the Blueprint — so the one
// namespace-native "did my authoring land?" verb confirmed nothing for a Blueprint
// target (the rich readback lived only in the actorName branch). The fix reads that
// state back off the compiled Blueprint (CDO member-variable defaults, the SCS sphere,
// and the GeneratedClass dispatchers) into the same shape the actor branch exposes.
// This authors a fresh in-code Actor Blueprint (no external asset) with a sphere, trace,
// chest props, and events via the REAL registered handlers, then asserts
// get_interaction_info({blueprintPath}) reports them. Reverting the readback makes the
// blueprintPath branch bare again and the section assertions below fail.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FInteractionGetInfoBlueprintReadbackTest,
    "PinWright.interaction.get_interaction_info.BlueprintPathReportsConfig",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FInteractionGetInfoBlueprintReadbackTest::RunTest(const FString& Parameters)
{
    // Build the fixture in-code (a fresh transient Actor Blueprint) — a missing fixture
    // is a failure, never a skip.
    UBlueprint* BP = CompilerTestUtils::CreateTransientTestBP(TEXT("GetInfoBlueprintReadback"));
    if (!TestNotNull(TEXT("interaction Blueprint fixture created"), BP))
    {
        return false;
    }
    const FString BlueprintPath = BP->GetPathName();

    // Author the interactable via the real registered handlers.
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("blueprintPath"), BlueprintPath);
        Payload->SetStringField(TEXT("componentName"), TEXT("InteractionSphere"));
        Payload->SetNumberField(TEXT("traceDistance"), 275.0);
        FTestResponseCapture Cap;
        TestTrue(TEXT("create_interaction_component found"),
            InvokeHandlerWithCapture(TEXT("interaction.create_interaction_component"), Payload, Cap));
        TestTrue(TEXT("create_interaction_component succeeded"), Cap.bSuccess);
    }
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("blueprintPath"), BlueprintPath);
        Payload->SetNumberField(TEXT("traceDistance"), 425.0);
        Payload->SetStringField(TEXT("traceType"), TEXT("sphere"));
        FTestResponseCapture Cap;
        TestTrue(TEXT("configure_interaction_trace found"),
            InvokeHandlerWithCapture(TEXT("interaction.configure_interaction_trace"), Payload, Cap));
        TestTrue(TEXT("configure_interaction_trace succeeded"), Cap.bSuccess);
    }
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("chestPath"), BlueprintPath);
        Payload->SetBoolField(TEXT("locked"), true);
        Payload->SetNumberField(TEXT("openAngle"), 110.0);
        Payload->SetNumberField(TEXT("openTime"), 0.75);
        FTestResponseCapture Cap;
        TestTrue(TEXT("configure_chest_properties found"),
            InvokeHandlerWithCapture(TEXT("interaction.configure_chest_properties"), Payload, Cap));
        TestTrue(TEXT("configure_chest_properties succeeded"), Cap.bSuccess);
    }
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("blueprintPath"), BlueprintPath);
        FTestResponseCapture Cap;
        TestTrue(TEXT("add_interaction_events found"),
            InvokeHandlerWithCapture(TEXT("interaction.add_interaction_events"), Payload, Cap));
        TestTrue(TEXT("add_interaction_events succeeded"), Cap.bSuccess);
    }

    // Read it back through get_interaction_info's blueprintPath branch — bare before the fix.
    TSharedPtr<FJsonObject> InfoPayload = MakeShared<FJsonObject>();
    InfoPayload->SetStringField(TEXT("blueprintPath"), BlueprintPath);
    FTestResponseCapture InfoCap;
    TestTrue(TEXT("get_interaction_info found"),
        InvokeHandlerWithCapture(TEXT("interaction.get_interaction_info"), InfoPayload, InfoCap));
    TestTrue(TEXT("get_interaction_info succeeded"), InfoCap.bSuccess);
    if (!TestTrue(TEXT("get_interaction_info returned a result"), InfoCap.Result.IsValid()))
    {
        return false;
    }

    // Name/path still present (pre-existing behavior).
    FString EchoedName;
    TestTrue(TEXT("blueprintName present"), InfoCap.Result->TryGetStringField(TEXT("blueprintName"), EchoedName));

    // (a) component: the interaction sphere with its baked radius.
    {
        const TSharedPtr<FJsonObject>* Component = nullptr;
        if (TestTrue(TEXT("readback reports a 'component' section"),
                InfoCap.Result->TryGetObjectField(TEXT("component"), Component) && Component && (*Component).IsValid()))
        {
            TestEqual(TEXT("component sphereRadius round-tripped"),
                (*Component)->GetNumberField(TEXT("sphereRadius")), 425.0);
        }
    }

    // (b) trace: TraceDistance baked onto the CDO by configure_interaction_trace.
    {
        const TSharedPtr<FJsonObject>* Trace = nullptr;
        if (TestTrue(TEXT("readback reports a 'trace' section"),
                InfoCap.Result->TryGetObjectField(TEXT("trace"), Trace) && Trace && (*Trace).IsValid()))
        {
            TestEqual(TEXT("trace distance round-tripped"),
                (*Trace)->GetNumberField(TEXT("traceDistance")), 425.0);
        }
    }

    // (e) chest gameplay props baked onto the CDO by configure_chest_properties.
    {
        const TSharedPtr<FJsonObject>* Chest = nullptr;
        if (TestTrue(TEXT("readback reports a 'chest' section"),
                InfoCap.Result->TryGetObjectField(TEXT("chest"), Chest) && Chest && (*Chest).IsValid()))
        {
            TestTrue(TEXT("chest locked round-tripped"), (*Chest)->GetBoolField(TEXT("locked")));
            TestEqual(TEXT("chest lidOpenAngle round-tripped"),
                (*Chest)->GetNumberField(TEXT("lidOpenAngle")), 110.0);
            TestEqual(TEXT("chest openTime round-tripped"),
                (*Chest)->GetNumberField(TEXT("openTime")), 0.75);
        }
    }

    // (d) events: the four interaction dispatchers on the GeneratedClass.
    {
        const TArray<TSharedPtr<FJsonValue>>* Events = nullptr;
        if (TestTrue(TEXT("readback reports an 'events' array"),
                InfoCap.Result->TryGetArrayField(TEXT("events"), Events) && Events))
        {
            TestEqual(TEXT("all four interaction events reported"), Events->Num(), 4);
        }
    }

    return true;
}
