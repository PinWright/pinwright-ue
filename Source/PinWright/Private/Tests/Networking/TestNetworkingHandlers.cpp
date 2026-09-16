// Copyright (c) 2026 Alexander Penkin. MIT License.

// Unit tests for Networking domain handlers
#include "Misc/AutomationTest.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/HandlerRegistration.h"
#include "Handlers/ParamSpec.h"
#include "Handlers/Networking/MovementPredictionUtils.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Tests/TestUtils.h"
#include "Tests/TestWorldUtils.h"
#include "Tests/TestSkipReporting.h"

#include "EditorAssetLibrary.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "K2Node_FunctionEntry.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"


// ============================================================================
// networking.set_property_replicated
// Required: blueprintPath, propertyName
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNetworkingSetPropertyReplicatedValidParamsNoCrashTest,
    "PinWright.networking.set_property_replicated.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNetworkingSetPropertyReplicatedValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("blueprintPath"), TEXT("/Game/Test/BP_TestActor"));
    Payload->SetStringField(TEXT("propertyName"), TEXT("Health"));
    Payload->SetBoolField(TEXT("replicated"), true);
    TestTrue(TEXT("Handler found and invoked"), InvokeHandler(TEXT("networking.set_property_replicated"), Payload));
    return true;
}

// ============================================================================
// networking.set_replication_condition
// Required: blueprintPath, propertyName, condition
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNetworkingSetReplicationConditionValidParamsNoCrashTest,
    "PinWright.networking.set_replication_condition.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNetworkingSetReplicationConditionValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("blueprintPath"), TEXT("/Game/Test/BP_TestActor"));
    Payload->SetStringField(TEXT("propertyName"), TEXT("Health"));
    Payload->SetStringField(TEXT("condition"), TEXT("COND_OwnerOnly"));
    TestTrue(TEXT("Handler found and invoked"), InvokeHandler(TEXT("networking.set_replication_condition"), Payload));
    return true;
}

// ============================================================================
// networking.configure_net_update_frequency
// Required: blueprintPath
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNetworkingConfigureNetUpdateFrequencyValidParamsNoCrashTest,
    "PinWright.networking.configure_net_update_frequency.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNetworkingConfigureNetUpdateFrequencyValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("blueprintPath"), TEXT("/Game/Test/BP_TestActor"));
    Payload->SetNumberField(TEXT("netUpdateFrequency"), 60.0);
    Payload->SetNumberField(TEXT("minNetUpdateFrequency"), 2.0);
    TestTrue(TEXT("Handler found and invoked"), InvokeHandler(TEXT("networking.configure_net_update_frequency"), Payload));
    return true;
}

// ============================================================================
// networking.configure_net_priority
// Required: blueprintPath
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNetworkingConfigureNetPriorityValidParamsNoCrashTest,
    "PinWright.networking.configure_net_priority.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNetworkingConfigureNetPriorityValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("blueprintPath"), TEXT("/Game/Test/BP_TestActor"));
    Payload->SetNumberField(TEXT("netPriority"), 2.0);
    TestTrue(TEXT("Handler found and invoked"), InvokeHandler(TEXT("networking.configure_net_priority"), Payload));
    return true;
}

// ============================================================================
// networking.set_net_dormancy
// Required: blueprintPath, dormancy
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNetworkingSetNetDormancyValidParamsNoCrashTest,
    "PinWright.networking.set_net_dormancy.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNetworkingSetNetDormancyValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("blueprintPath"), TEXT("/Game/Test/BP_TestActor"));
    Payload->SetStringField(TEXT("dormancy"), TEXT("DORM_Awake"));
    TestTrue(TEXT("Handler found and invoked"), InvokeHandler(TEXT("networking.set_net_dormancy"), Payload));
    return true;
}

// ============================================================================
// networking.create_rpc_function
// Required: blueprintPath, functionName, rpcType
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNetworkingCreateRpcFunctionValidParamsNoCrashTest,
    "PinWright.networking.create_rpc_function.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNetworkingCreateRpcFunctionValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("blueprintPath"), TEXT("/Game/Test/BP_TestActor"));
    Payload->SetStringField(TEXT("functionName"), TEXT("ServerDoThing"));
    Payload->SetStringField(TEXT("rpcType"), TEXT("Server"));
    Payload->SetBoolField(TEXT("reliable"), true);
    TestTrue(TEXT("Handler found and invoked"), InvokeHandler(TEXT("networking.create_rpc_function"), Payload));
    return true;
}

// ============================================================================
// networking.configure_rpc_validation
// Required: blueprintPath, functionName
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNetworkingConfigureRpcValidationValidParamsNoCrashTest,
    "PinWright.networking.configure_rpc_validation.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNetworkingConfigureRpcValidationValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("blueprintPath"), TEXT("/Game/Test/BP_TestActor"));
    Payload->SetStringField(TEXT("functionName"), TEXT("ServerDoThing"));
    Payload->SetBoolField(TEXT("withValidation"), false);
    TestTrue(TEXT("Handler found and invoked"), InvokeHandler(TEXT("networking.configure_rpc_validation"), Payload));
    return true;
}

// ============================================================================
// networking.set_rpc_reliability
// Required: blueprintPath, functionName
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNetworkingSetRpcReliabilityValidParamsNoCrashTest,
    "PinWright.networking.set_rpc_reliability.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNetworkingSetRpcReliabilityValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("blueprintPath"), TEXT("/Game/Test/BP_TestActor"));
    Payload->SetStringField(TEXT("functionName"), TEXT("ServerDoThing"));
    Payload->SetBoolField(TEXT("reliable"), false);
    TestTrue(TEXT("Handler found and invoked"), InvokeHandler(TEXT("networking.set_rpc_reliability"), Payload));
    return true;
}

// ============================================================================
// networking.set_owner
// Required: actorName
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNetworkingSetOwnerValidParamsNoCrashTest,
    "PinWright.networking.set_owner.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNetworkingSetOwnerValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("actorName"), TEXT("BP_TestActor_0"));
    Payload->SetStringField(TEXT("ownerActorName"), TEXT("BP_PlayerController_0"));
    TestTrue(TEXT("Handler found and invoked"), InvokeHandler(TEXT("networking.set_owner"), Payload));
    return true;
}

// ============================================================================
// networking.set_owner — resolve actorName by display label (not internal name only)
// (E-networking-actorname-internal-name-only)
// Regression: the networking actor verbs (set_owner / check_has_authority /
// check_is_locally_controlled / get_networking_info actor branch) resolved actorName
// through a private FindActorByName that matched GetName() ONLY — case-sensitive,
// rejecting the display label that actor.spawn/actor.list accept AND that set_owner's
// own success verification echoes back as actorName (AddActorVerification writes
// GetActorLabel()). So re-using the echoed actorName round-tripped to [NOT_FOUND].
// The fix routes the resolver through McpActorUtils::FindActorByNameSimple (label OR
// internal name OR path), matching the actor.* family and the focus_actor precedent.
//
// This spawns a real actor and gives it a display label deliberately DIFFERENT from
// its internal object name, drives the production set_owner handler with the LABEL,
// and asserts success + that the verification echoes back that same label as
// actorName (proving the input value the handler resolved IS what it reports). Under
// the reverted GetName()-only helper, the distinct label never equals GetName(), so
// the handler returns [NOT_FOUND] and the success assertion fails.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNetworkingSetOwnerResolvesDisplayLabelTest,
    "PinWright.networking.set_owner.ResolvesDisplayLabel",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNetworkingSetOwnerResolvesDisplayLabelTest::RunTest(const FString& Parameters)
{
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("No editor world available; skipping set_owner display-label resolution test."));
        return true;
    }

    // The guard destroys probes spawned during the test and restores the map's dirty
    // flag on every exit path, so the open map is left untouched.
    FScopedEditorWorldActorGuard WorldGuard;

    // Spawn the probe actors via the shared transient-cube helper (RF_Transient +
    // SetActorLabel in one call), passing each a display label that differs from its
    // internal object name (GetName()), so the only branch that can resolve the label
    // is the label-matching path under test.
    AActor* Probe = SpawnTransientCubeActor(World, TEXT("NetSetOwnerProbeDistinctLabel"), FVector::ZeroVector);
    AActor* OwnerProbe = SpawnTransientCubeActor(World, TEXT("NetSetOwnerProbeOwnerDistinctLabel"), FVector(0.f, 0.f, 200.f));
    TestNotNull(TEXT("probe actor spawned"), Probe);
    TestNotNull(TEXT("owner probe actor spawned"), OwnerProbe);
    if (!Probe || !OwnerProbe)
    {
        return true;
    }

    const FString InternalName = Probe->GetName();
    const FString DisplayLabel = Probe->GetActorLabel();
    const FString OwnerDisplayLabel = OwnerProbe->GetActorLabel();

    // Sanity: the label and the internal name must differ, otherwise the bug could be
    // masked by the label happening to equal GetName().
    TestNotEqual(TEXT("internal name differs from display label"), InternalName, DisplayLabel);

    // Drive the production handler with the DISPLAY LABELS (not the internal names).
    FTestResponseCapture Capture;
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("actorName"), DisplayLabel);
    Payload->SetStringField(TEXT("ownerActorName"), OwnerDisplayLabel);
    const bool bFound = InvokeHandlerWithCapture(TEXT("networking.set_owner"), Payload, Capture);
    TestTrue(TEXT("networking.set_owner handler found"), bFound);

    // Core assertion: resolving by the display label succeeds (it returned [NOT_FOUND]
    // before the fix because the private helper matched GetName() only).
    TestTrue(TEXT("set_owner resolves the display label"), Capture.bSuccess);

    if (Capture.bSuccess)
    {
        // The owner write must have actually landed on the actor identified by the label.
        TestEqual(TEXT("set_owner applied the owner resolved by label"),
            Probe->GetOwner(), OwnerProbe);

        // The success verification echoes actorName=<label>; confirm the value the
        // handler reports back is exactly the label it just resolved from input — i.e.
        // the round-trip no longer contradicts itself.
        if (Capture.Result.IsValid())
        {
            FString EchoedActorName;
            TestTrue(TEXT("response carries actorName"),
                Capture.Result->TryGetStringField(TEXT("actorName"), EchoedActorName));
            TestEqual(TEXT("echoed actorName round-trips the resolved label"),
                EchoedActorName, DisplayLabel);
        }
    }

    return true;
}

// ============================================================================
// networking.set_autonomous_proxy
// Required: blueprintPath
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNetworkingSetAutonomousProxyValidParamsNoCrashTest,
    "PinWright.networking.set_autonomous_proxy.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNetworkingSetAutonomousProxyValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("blueprintPath"), TEXT("/Game/Test/BP_TestActor"));
    Payload->SetBoolField(TEXT("isAutonomousProxy"), true);
    TestTrue(TEXT("Handler found and invoked"), InvokeHandler(TEXT("networking.set_autonomous_proxy"), Payload));
    return true;
}

// ============================================================================
// networking.check_has_authority
// Required: actorName
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNetworkingCheckHasAuthorityValidParamsNoCrashTest,
    "PinWright.networking.check_has_authority.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNetworkingCheckHasAuthorityValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("actorName"), TEXT("BP_TestActor_0"));
    TestTrue(TEXT("Handler found and invoked"), InvokeHandler(TEXT("networking.check_has_authority"), Payload));
    return true;
}

// ============================================================================
// networking.check_is_locally_controlled
// Required: actorName
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNetworkingCheckIsLocallyControlledValidParamsNoCrashTest,
    "PinWright.networking.check_is_locally_controlled.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNetworkingCheckIsLocallyControlledValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("actorName"), TEXT("BP_TestPawn_0"));
    TestTrue(TEXT("Handler found and invoked"), InvokeHandler(TEXT("networking.check_is_locally_controlled"), Payload));
    return true;
}

// ============================================================================
// networking.configure_net_cull_distance
// Required: blueprintPath
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNetworkingConfigureNetCullDistanceValidParamsNoCrashTest,
    "PinWright.networking.configure_net_cull_distance.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNetworkingConfigureNetCullDistanceValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("blueprintPath"), TEXT("/Game/Test/BP_TestActor"));
    Payload->SetNumberField(TEXT("netCullDistanceSquared"), 225000000.0);
    Payload->SetBoolField(TEXT("useOwnerNetRelevancy"), false);
    TestTrue(TEXT("Handler found and invoked"), InvokeHandler(TEXT("networking.configure_net_cull_distance"), Payload));
    return true;
}

// ============================================================================
// networking.set_always_relevant
// Required: blueprintPath
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNetworkingSetAlwaysRelevantValidParamsNoCrashTest,
    "PinWright.networking.set_always_relevant.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNetworkingSetAlwaysRelevantValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("blueprintPath"), TEXT("/Game/Test/BP_TestActor"));
    Payload->SetBoolField(TEXT("alwaysRelevant"), true);
    TestTrue(TEXT("Handler found and invoked"), InvokeHandler(TEXT("networking.set_always_relevant"), Payload));
    return true;
}

// ============================================================================
// networking.set_only_relevant_to_owner
// Required: blueprintPath
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNetworkingSetOnlyRelevantToOwnerValidParamsNoCrashTest,
    "PinWright.networking.set_only_relevant_to_owner.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNetworkingSetOnlyRelevantToOwnerValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("blueprintPath"), TEXT("/Game/Test/BP_TestActor"));
    Payload->SetBoolField(TEXT("onlyRelevantToOwner"), true);
    TestTrue(TEXT("Handler found and invoked"), InvokeHandler(TEXT("networking.set_only_relevant_to_owner"), Payload));
    return true;
}

// ============================================================================
// networking.set_replicated_using
// Required: blueprintPath, propertyName, repNotifyFunc
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNetworkingSetReplicatedUsingValidParamsNoCrashTest,
    "PinWright.networking.set_replicated_using.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNetworkingSetReplicatedUsingValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("blueprintPath"), TEXT("/Game/Test/BP_TestActor"));
    Payload->SetStringField(TEXT("propertyName"), TEXT("Health"));
    Payload->SetStringField(TEXT("repNotifyFunc"), TEXT("OnRep_Health"));
    TestTrue(TEXT("Handler found and invoked"), InvokeHandler(TEXT("networking.set_replicated_using"), Payload));
    return true;
}

// ============================================================================
// networking.configure_client_prediction
// Required: blueprintPath
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNetworkingConfigureClientPredictionValidParamsNoCrashTest,
    "PinWright.networking.configure_client_prediction.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNetworkingConfigureClientPredictionValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("blueprintPath"), TEXT("/Game/Test/BP_TestCharacter"));
    Payload->SetBoolField(TEXT("enablePrediction"), true);
    Payload->SetNumberField(TEXT("predictionThreshold"), 0.1);
    TestTrue(TEXT("Handler found and invoked"), InvokeHandler(TEXT("networking.configure_client_prediction"), Payload));
    return true;
}

// ============================================================================
// networking.configure_server_correction
// Required: blueprintPath
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNetworkingConfigureServerCorrectionValidParamsNoCrashTest,
    "PinWright.networking.configure_server_correction.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNetworkingConfigureServerCorrectionValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("blueprintPath"), TEXT("/Game/Test/BP_TestCharacter"));
    Payload->SetNumberField(TEXT("correctionThreshold"), 1.0);
    Payload->SetNumberField(TEXT("smoothingRate"), 0.5);
    TestTrue(TEXT("Handler found and invoked"), InvokeHandler(TEXT("networking.configure_server_correction"), Payload));
    return true;
}

// ============================================================================
// networking.add_network_prediction_data
// Required: blueprintPath, dataType
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNetworkingAddNetworkPredictionDataValidParamsNoCrashTest,
    "PinWright.networking.add_network_prediction_data.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNetworkingAddNetworkPredictionDataValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("blueprintPath"), TEXT("/Game/Test/BP_TestActor"));
    Payload->SetStringField(TEXT("dataType"), TEXT("Transform"));
    Payload->SetStringField(TEXT("variableName"), TEXT("PredictedTransform"));
    TestTrue(TEXT("Handler found and invoked"), InvokeHandler(TEXT("networking.add_network_prediction_data"), Payload));
    return true;
}

// ============================================================================
// networking.configure_movement_prediction
// Required: blueprintPath
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNetworkingConfigureMovementPredictionValidParamsNoCrashTest,
    "PinWright.networking.configure_movement_prediction.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNetworkingConfigureMovementPredictionValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("blueprintPath"), TEXT("/Game/Test/BP_TestCharacter"));
    Payload->SetStringField(TEXT("networkSmoothingMode"), TEXT("Exponential"));
    Payload->SetNumberField(TEXT("networkMaxSmoothUpdateDistance"), 256.0);
    Payload->SetNumberField(TEXT("networkNoSmoothUpdateDistance"), 384.0);
    TestTrue(TEXT("Handler found and invoked"), InvokeHandler(TEXT("networking.configure_movement_prediction"), Payload));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNetworkingConfigureMovementPredictionSettingsUnitTest,
    "PinWright.networking.configure_movement_prediction.SmoothingModeContract",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNetworkingConfigureMovementPredictionSettingsUnitTest::RunTest(const FString& Parameters)
{
    ENetworkSmoothingMode ParsedMode = ENetworkSmoothingMode::Exponential;
    TestTrue(TEXT("Disabled mode parses"),
        PinWrightMovementPrediction::TryParseNetworkSmoothingMode(TEXT("Disabled"), ParsedMode));
    TestTrue(TEXT("Disabled mode maps to the engine enum"),
        ParsedMode == ENetworkSmoothingMode::Disabled);
    TestTrue(TEXT("Linear mode parses case-insensitively"),
        PinWrightMovementPrediction::TryParseNetworkSmoothingMode(TEXT("linear"), ParsedMode));
    TestTrue(TEXT("Linear mode maps to the engine enum"),
        ParsedMode == ENetworkSmoothingMode::Linear);
    TestTrue(TEXT("Exponential mode parses"),
        PinWrightMovementPrediction::TryParseNetworkSmoothingMode(TEXT("Exponential"), ParsedMode));
    TestTrue(TEXT("Exponential mode maps to the engine enum"),
        ParsedMode == ENetworkSmoothingMode::Exponential);
    TestFalse(TEXT("unknown smoothing mode is rejected"),
        PinWrightMovementPrediction::TryParseNetworkSmoothingMode(TEXT("Cubic"), ParsedMode));

    UCharacterMovementComponent* Movement = NewObject<UCharacterMovementComponent>();
    if (TestNotNull(TEXT("transient movement component created"), Movement))
    {
        PinWrightMovementPrediction::ApplyMovementPredictionSettings(*Movement,
            ENetworkSmoothingMode::Linear, 123.0f, 456.0f);
        TestTrue(TEXT("smoothing mode is applied to the movement component"),
            Movement->NetworkSmoothingMode == ENetworkSmoothingMode::Linear);
        TestEqual(TEXT("max smooth update distance is applied"),
            Movement->NetworkMaxSmoothUpdateDistance, 123.0f);
        TestEqual(TEXT("no-smooth update distance is applied"),
            Movement->NetworkNoSmoothUpdateDistance, 456.0f);
    }

    TSharedPtr<FJsonObject> InvalidPayload = MakeShared<FJsonObject>();
    InvalidPayload->SetStringField(TEXT("blueprintPath"), TEXT("/Game/Test/BP_DoesNotExist"));
    InvalidPayload->SetStringField(TEXT("networkSmoothingMode"), TEXT("Cubic"));
    FTestResponseCapture Capture;
    TestTrue(TEXT("configure_movement_prediction handler found"),
        InvokeHandlerWithCapture(TEXT("networking.configure_movement_prediction"),
            InvalidPayload, Capture));
    TestFalse(TEXT("unknown smoothing mode returns an error"), Capture.bSuccess);
    TestEqual(TEXT("unknown smoothing mode is rejected before asset load"),
        Capture.ErrorCode, FString(TEXT("INVALID_ARGUMENT")));
    return true;
}

// ============================================================================
// networking.configure_replicated_movement
// Required: blueprintPath
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNetworkingConfigureReplicatedMovementValidParamsNoCrashTest,
    "PinWright.networking.configure_replicated_movement.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNetworkingConfigureReplicatedMovementValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("blueprintPath"), TEXT("/Game/Test/BP_TestActor"));
    Payload->SetBoolField(TEXT("replicateMovement"), true);
    TestTrue(TEXT("Handler found and invoked"), InvokeHandler(TEXT("networking.configure_replicated_movement"), Payload));
    return true;
}

// ============================================================================
// networking.get_networking_info
// All params optional — one ValidParamsNoCrash test is sufficient
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNetworkingGetNetworkingInfoValidParamsNoCrashTest,
    "PinWright.networking.get_networking_info.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNetworkingGetNetworkingInfoValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("blueprintPath"), TEXT("/Game/Test/BP_TestActor"));
    TestTrue(TEXT("Handler found and invoked"), InvokeHandler(TEXT("networking.get_networking_info"), Payload));
    return true;
}

// ============================================================================
// networking.get_networking_info — RPC + replicated-property round-trip
// (F-networking-info-no-rpc-detail)
// End-to-end regression: build a real package-backed blueprint, drive the
// networking setters (create_rpc_function + set_rpc_reliability;
// set_property_replicated + set_replicated_using + set_replication_condition),
// then read back via get_networking_info and assert the reader surfaces the
// per-RPC and per-property detail. The load-bearing fields here are the ones no
// other reader exposes: rpcFunctions[].withValidation, replicatedProperties[].
// replicatedUsing (the RepNotify function name), and replicationCondition.
// Fails if the reader extension is reverted or stops reporting a legacy
// validation flag that remains present on an existing function.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNetworkingGetNetworkingInfoRpcReplicationRoundTripTest,
    "PinWright.networking.get_networking_info.RpcReplicationRoundTrip",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNetworkingGetNetworkingInfoRpcReplicationRoundTripTest::RunTest(const FString& Parameters)
{
    // 1. Create a real package-backed blueprint loadable by path.
    const FString AssetPath = FString::Printf(
        TEXT("/Game/__PW_GatewayTests/BP_NetInfo_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    UPackage* Pkg = CreatePackage(*AssetPath);
    UBlueprint* BP = FKismetEditorUtilities::CreateBlueprint(
        AActor::StaticClass(), Pkg,
        FName(*FPackageName::GetLongPackageAssetName(AssetPath)),
        BPTYPE_Normal, UBlueprint::StaticClass(),
        UBlueprintGeneratedClass::StaticClass());

    if (!TestNotNull(TEXT("Blueprint created"), BP))
    {
        CleanupTestAsset(AssetPath);
        return true;
    }

    // 2a. Add a replicated variable + RepNotify + replication condition.
    // Build the fixture variable directly and unconditionally — add_variable is not
    // the unit under test (get_networking_info's readback is), so the deterministic
    // AddMemberVariable path keeps the test self-contained and focused on the reader.
    {
        FEdGraphPinType IntPin;
        IntPin.PinCategory = UEdGraphSchema_K2::PC_Int;
        FBlueprintEditorUtils::AddMemberVariable(BP, FName(TEXT("Health")), IntPin);
    }

    {
        TSharedPtr<FJsonObject> SetRep = MakeShared<FJsonObject>();
        SetRep->SetStringField(TEXT("blueprintPath"), AssetPath);
        SetRep->SetStringField(TEXT("propertyName"), TEXT("Health"));
        SetRep->SetBoolField(TEXT("replicated"), true);
        InvokeHandler(TEXT("networking.set_property_replicated"), SetRep);
    }
    {
        TSharedPtr<FJsonObject> SetRepUsing = MakeShared<FJsonObject>();
        SetRepUsing->SetStringField(TEXT("blueprintPath"), AssetPath);
        SetRepUsing->SetStringField(TEXT("propertyName"), TEXT("Health"));
        SetRepUsing->SetStringField(TEXT("repNotifyFunc"), TEXT("OnRep_Health"));
        InvokeHandler(TEXT("networking.set_replicated_using"), SetRepUsing);
    }
    {
        TSharedPtr<FJsonObject> SetCond = MakeShared<FJsonObject>();
        SetCond->SetStringField(TEXT("blueprintPath"), AssetPath);
        SetCond->SetStringField(TEXT("propertyName"), TEXT("Health"));
        SetCond->SetStringField(TEXT("condition"), TEXT("COND_OwnerOnly"));
        InvokeHandler(TEXT("networking.set_replication_condition"), SetCond);
    }

    // 2b. Create a reliable Server RPC. Blueprint-authored RPCs cannot carry
    // FUNC_NetValidate because UE implements validation through native thunks.
    {
        TSharedPtr<FJsonObject> CreateRpc = MakeShared<FJsonObject>();
        CreateRpc->SetStringField(TEXT("blueprintPath"), AssetPath);
        CreateRpc->SetStringField(TEXT("functionName"), TEXT("ServerApplyDamage"));
        CreateRpc->SetStringField(TEXT("rpcType"), TEXT("Server"));
        CreateRpc->SetBoolField(TEXT("reliable"), true);
        InvokeHandler(TEXT("networking.create_rpc_function"), CreateRpc);
    }
    {
        TSharedPtr<FJsonObject> SetReliable = MakeShared<FJsonObject>();
        SetReliable->SetStringField(TEXT("blueprintPath"), AssetPath);
        SetReliable->SetStringField(TEXT("functionName"), TEXT("ServerApplyDamage"));
        SetReliable->SetBoolField(TEXT("reliable"), true);
        InvokeHandler(TEXT("networking.set_rpc_reliability"), SetReliable);
    }

    // Model a legacy/stale function that already carries the flag. Authoring it
    // is unsupported, but get_networking_info must still report existing state.
    for (UEdGraph* Graph : BP->FunctionGraphs)
    {
        if (!Graph || Graph->GetFName() != FName(TEXT("ServerApplyDamage")))
        {
            continue;
        }
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (UK2Node_FunctionEntry* EntryNode = Cast<UK2Node_FunctionEntry>(Node))
            {
                EntryNode->AddExtraFlags(FUNC_NetValidate);
                break;
            }
        }
        break;
    }

    // 3. Read back via the production handler and capture the response.
    FTestResponseCapture Capture;
    TSharedPtr<FJsonObject> InfoPayload = MakeShared<FJsonObject>();
    InfoPayload->SetStringField(TEXT("blueprintPath"), AssetPath);
    const bool bFound = InvokeHandlerWithCapture(
        TEXT("networking.get_networking_info"), InfoPayload, Capture);
    TestTrue(TEXT("get_networking_info handler found"), bFound);
    TestTrue(TEXT("get_networking_info succeeded"), Capture.bSuccess);

    TSharedPtr<FJsonObject> NetworkingInfo;
    if (Capture.Result.IsValid())
    {
        const TSharedPtr<FJsonObject>* InfoPtr = nullptr;
        if (Capture.Result->TryGetObjectField(TEXT("networkingInfo"), InfoPtr) && InfoPtr)
        {
            NetworkingInfo = *InfoPtr;
        }
    }
    TestTrue(TEXT("networkingInfo object present"), NetworkingInfo.IsValid());

    // 4. The RPC reader must report both authored flags and the directly
    // injected legacy validation flag.
    TSharedPtr<FJsonObject> RpcEntry = JsonArrayFindObjectByStringField(
        NetworkingInfo, TEXT("rpcFunctions"), TEXT("name"), TEXT("ServerApplyDamage"));
    if (TestNotNull(TEXT("rpcFunctions contains ServerApplyDamage"), RpcEntry.Get()))
    {
        FString RpcType;
        RpcEntry->TryGetStringField(TEXT("rpcType"), RpcType);
        TestEqual(TEXT("ServerApplyDamage rpcType is Server"), RpcType, FString(TEXT("Server")));

        bool bReliable = false;
        RpcEntry->TryGetBoolField(TEXT("reliable"), bReliable);
        TestTrue(TEXT("ServerApplyDamage reliable round-trips"), bReliable);

        bool bWithValidation = false;
        RpcEntry->TryGetBoolField(TEXT("withValidation"), bWithValidation);
        TestTrue(TEXT("Legacy ServerApplyDamage validation flag is reported"), bWithValidation);
    }

    // 5. The replicated property must round-trip its RepNotify function NAME and
    //    its replication CONDITION (fields no other reader surfaces).
    TSharedPtr<FJsonObject> PropEntry = JsonArrayFindObjectByStringField(
        NetworkingInfo, TEXT("replicatedProperties"), TEXT("name"), TEXT("Health"));
    if (TestNotNull(TEXT("replicatedProperties contains Health"), PropEntry.Get()))
    {
        FString RepUsing;
        PropEntry->TryGetStringField(TEXT("replicatedUsing"), RepUsing);
        TestEqual(TEXT("Health replicatedUsing round-trips the RepNotify name"),
            RepUsing, FString(TEXT("OnRep_Health")));

        FString Condition;
        PropEntry->TryGetStringField(TEXT("replicationCondition"), Condition);
        TestEqual(TEXT("Health replicationCondition round-trips"),
            Condition, FString(TEXT("COND_OwnerOnly")));
    }

    // 6. Cleanup.
    CleanupTestAsset(AssetPath);
    return true;
}

// ============================================================================
// networking.set_property_replicated — bReplicates dependency warning
// (E-set-property-replicated-no-actor-replicates-flag)
// End-to-end regression: marking a property replicated on an actor whose CDO
// still has bReplicates=false must surface the dependency in the SUCCESS
// response (bReplicatesEnabled:false + a warning naming the follow-up verb),
// because a CPF_Net property on a non-replicating actor is functionally inert.
// After enabling actor replication via misc.set_replication, a second call must
// report bReplicatesEnabled:true and carry NO warning. The handler must NOT
// mutate bReplicates itself (the rejected auto-enable). Fails if the warning /
// bReplicatesEnabled surfacing is reverted, OR if the setter starts silently
// flipping bReplicates on its own.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNetworkingSetPropertyReplicatedWarnsWhenActorNotReplicatingTest,
    "PinWright.networking.set_property_replicated.WarnsWhenActorNotReplicating",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNetworkingSetPropertyReplicatedWarnsWhenActorNotReplicatingTest::RunTest(const FString& Parameters)
{
    // 1. Create a real package-backed actor blueprint (CDO bReplicates defaults false).
    const FString AssetPath = FString::Printf(
        TEXT("/Game/__PW_GatewayTests/BP_RepWarn_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    UPackage* Pkg = CreatePackage(*AssetPath);
    UBlueprint* BP = FKismetEditorUtilities::CreateBlueprint(
        AActor::StaticClass(), Pkg,
        FName(*FPackageName::GetLongPackageAssetName(AssetPath)),
        BPTYPE_Normal, UBlueprint::StaticClass(),
        UBlueprintGeneratedClass::StaticClass());

    if (!TestNotNull(TEXT("Blueprint created"), BP))
    {
        CleanupTestAsset(AssetPath);
        return true;
    }

    // 2. Add a member variable to mark replicated, then compile so the CDO
    //    materializes (its bReplicates defaults false). set_property_replicated
    //    walks Blueprint->NewVariables and compiles the blueprint itself, so the
    //    CPF_Net flag persists across recompile — exactly as a real on-disk
    //    blueprint would.
    {
        FEdGraphPinType IntPin;
        IntPin.PinCategory = UEdGraphSchema_K2::PC_Int;
        FBlueprintEditorUtils::AddMemberVariable(BP, FName(TEXT("HealAmount")), IntPin);
        FKismetEditorUtilities::CompileBlueprint(BP);
    }

    // 3. Mark it replicated while the actor still does NOT replicate. The response
    //    must surface the dependency: bReplicatesEnabled:false + a warning.
    {
        FTestResponseCapture Capture;
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("blueprintPath"), AssetPath);
        Payload->SetStringField(TEXT("propertyName"), TEXT("HealAmount"));
        Payload->SetBoolField(TEXT("replicated"), true);
        const bool bFound = InvokeHandlerWithCapture(
            TEXT("networking.set_property_replicated"), Payload, Capture);
        TestTrue(TEXT("set_property_replicated handler found"), bFound);
        TestTrue(TEXT("set_property_replicated succeeded"), Capture.bSuccess);

        if (TestTrue(TEXT("response captured"), Capture.Result.IsValid()))
        {
            bool bReplicatesEnabled = true;
            const bool bHasField = Capture.Result->TryGetBoolField(
                TEXT("bReplicatesEnabled"), bReplicatesEnabled);
            TestTrue(TEXT("bReplicatesEnabled field present"), bHasField);
            TestFalse(TEXT("bReplicatesEnabled is false (actor still non-replicating)"),
                bReplicatesEnabled);

            FString Warning;
            const bool bHasWarning = Capture.Result->TryGetStringField(TEXT("warning"), Warning);
            TestTrue(TEXT("warning present when actor does not replicate"), bHasWarning);
            TestFalse(TEXT("warning is non-empty"), Warning.IsEmpty());
        }

        // The handler must NOT have flipped bReplicates on its own (rejected auto-enable).
        AActor* CDO = Cast<AActor>(BP->GeneratedClass->GetDefaultObject());
        if (TestNotNull(TEXT("actor CDO resolvable"), CDO))
        {
            TestFalse(TEXT("set_property_replicated did not auto-enable bReplicates"),
                CDO->GetIsReplicated());
        }

        // Fixed behavior: CPF_Net lands on the PERSISTENT variable description
        // (NewVariables), not just the transient compiled FProperty, so it survives
        // the next recompile.
        bool bNetFlagPersisted = false;
        for (const FBPVariableDescription& VarDesc : BP->NewVariables)
        {
            if (VarDesc.VarName == FName(TEXT("HealAmount")))
            {
                bNetFlagPersisted = (VarDesc.PropertyFlags & CPF_Net) != 0;
                break;
            }
        }
        TestTrue(TEXT("CPF_Net written to NewVariables (persists across recompile)"), bNetFlagPersisted);
    }

    // 4. Enable actor replication out-of-band (the documented follow-up verb).
    {
        TSharedPtr<FJsonObject> SetRep = MakeShared<FJsonObject>();
        SetRep->SetStringField(TEXT("blueprintPath"), AssetPath);
        SetRep->SetBoolField(TEXT("replicates"), true);
        SetRep->SetBoolField(TEXT("replicateMovement"), false);
        TestTrue(TEXT("misc.set_replication handler found"),
            InvokeHandler(TEXT("misc.set_replication"), SetRep));
    }

    // 5. Re-mark the property replicated. Now the actor replicates, so the
    //    response must report bReplicatesEnabled:true and carry NO warning.
    {
        FTestResponseCapture Capture;
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("blueprintPath"), AssetPath);
        Payload->SetStringField(TEXT("propertyName"), TEXT("HealAmount"));
        Payload->SetBoolField(TEXT("replicated"), true);
        const bool bFound = InvokeHandlerWithCapture(
            TEXT("networking.set_property_replicated"), Payload, Capture);
        TestTrue(TEXT("second set_property_replicated handler found"), bFound);
        TestTrue(TEXT("second set_property_replicated succeeded"), Capture.bSuccess);

        if (TestTrue(TEXT("second response captured"), Capture.Result.IsValid()))
        {
            bool bReplicatesEnabled = false;
            Capture.Result->TryGetBoolField(TEXT("bReplicatesEnabled"), bReplicatesEnabled);
            TestTrue(TEXT("bReplicatesEnabled is true after misc.set_replication"),
                bReplicatesEnabled);

            FString Warning;
            const bool bHasWarning = Capture.Result->TryGetStringField(TEXT("warning"), Warning);
            TestFalse(TEXT("no warning once the actor replicates"), bHasWarning);
        }
    }

    // 6. Cleanup.
    CleanupTestAsset(AssetPath);
    return true;
}

// ============================================================================
// networking.create_rpc_function — parameter-pin authoring (E-create-rpc-function-no-param-slot)
// End-to-end regression: create a real package-backed blueprint, create a
// reliable Server RPC with an inputs={DamageAmount:float} pin, then verify the
// function entry node carries BOTH the named input pin AND the surviving net
// flags (FUNC_Net / FUNC_NetReliable / FUNC_NetServer). Fails if the inputs
// handling is reverted (the old handler built an empty-signature RPC).
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNetworkingCreateRpcFunctionInputParamPinCreatedTest,
    "PinWright.networking.create_rpc_function.InputParamPinCreated",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNetworkingCreateRpcFunctionInputParamPinCreatedTest::RunTest(const FString& Parameters)
{
    // 1. Create a real package-backed blueprint loadable by path.
    const FString AssetPath = FString::Printf(
        TEXT("/Game/__PW_GatewayTests/BP_RpcParam_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    UPackage* Pkg = CreatePackage(*AssetPath);
    UBlueprint* BP = FKismetEditorUtilities::CreateBlueprint(
        AActor::StaticClass(), Pkg,
        FName(*FPackageName::GetLongPackageAssetName(AssetPath)),
        BPTYPE_Normal, UBlueprint::StaticClass(),
        UBlueprintGeneratedClass::StaticClass());

    if (!TestNotNull(TEXT("Blueprint created"), BP))
    {
        CleanupTestAsset(AssetPath);
        return true;
    }

    // 2. Create a reliable Server RPC WITH a float input parameter.
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("blueprintPath"), AssetPath);
    Payload->SetStringField(TEXT("functionName"), TEXT("ServerApplyDamage"));
    Payload->SetStringField(TEXT("rpcType"), TEXT("Server"));
    Payload->SetBoolField(TEXT("reliable"), true);

    TArray<TSharedPtr<FJsonValue>> InputsArray;
    TSharedPtr<FJsonObject> InputObj = MakeShared<FJsonObject>();
    InputObj->SetStringField(TEXT("name"), TEXT("DamageAmount"));
    InputObj->SetStringField(TEXT("type"), TEXT("float"));
    InputsArray.Add(MakeShared<FJsonValueObject>(InputObj));
    Payload->SetArrayField(TEXT("inputs"), InputsArray);

    FTestResponseCapture Capture;
    const bool bFound = InvokeHandlerWithCapture(TEXT("networking.create_rpc_function"), Payload, Capture);
    TestTrue(TEXT("Handler found"), bFound);
    TestTrue(TEXT("Handler succeeded"), Capture.bSuccess);

    // 3. Find the new function graph + entry node and assert both the input pin
    //    and the net flags landed on it.
    UEdGraph* FuncGraph = nullptr;
    for (UEdGraph* Graph : BP->FunctionGraphs)
    {
        if (Graph && Graph->GetFName() == FName(TEXT("ServerApplyDamage")))
        {
            FuncGraph = Graph;
            break;
        }
    }
    if (TestNotNull(TEXT("ServerApplyDamage function graph created"), FuncGraph))
    {
        UK2Node_FunctionEntry* EntryNode = nullptr;
        for (UEdGraphNode* Node : FuncGraph->Nodes)
        {
            if (UK2Node_FunctionEntry* Entry = Cast<UK2Node_FunctionEntry>(Node))
            {
                EntryNode = Entry;
                break;
            }
        }
        if (TestNotNull(TEXT("Function entry node exists"), EntryNode))
        {
            // The input parameter pin must exist on the entry node — this is the
            // capability the fix adds; without it the RPC is parameterless.
            UEdGraphPin* DamagePin = nullptr;
            for (UEdGraphPin* Pin : EntryNode->Pins)
            {
                if (Pin && Pin->Direction == EGPD_Output
                    && Pin->PinName == FName(TEXT("DamageAmount")))
                {
                    DamagePin = Pin;
                    break;
                }
            }
            TestNotNull(TEXT("DamageAmount input pin created on RPC entry node"), DamagePin);
            if (DamagePin)
            {
                const bool bIsFloat =
                    DamagePin->PinType.PinCategory == UEdGraphSchema_K2::PC_Real
                    || DamagePin->PinType.PinCategory == UEdGraphSchema_K2::PC_Float;
                TestTrue(TEXT("DamageAmount pin is a floating-point type"), bIsFloat);
            }

            // The net flags must still be present (the parameter work must not
            // displace the RPC stamping).
            const int32 Flags = EntryNode->GetExtraFlags();
            TestTrue(TEXT("FUNC_Net set"), (Flags & FUNC_Net) != 0);
            TestTrue(TEXT("FUNC_NetReliable set"), (Flags & FUNC_NetReliable) != 0);
            TestTrue(TEXT("FUNC_NetServer set"), (Flags & FUNC_NetServer) != 0);
        }
    }

    // 4. Cleanup.
    CleanupTestAsset(AssetPath);
    return true;
}
