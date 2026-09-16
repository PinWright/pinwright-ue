// Copyright (c) 2026 Alexander Penkin. MIT License.

// Tests for the typed Chaos vehicle authoring handlers. Whole TU is gated on
// ChaosVehicles plugin presence; on engines without it the file is empty.
//
// Counterfactual: if the wheel-property reflection apply in
// vehicle.create_wheel_asset is reverted/skipped, the WheelRadius == 42.0f
// assertion in the round-trip test fails because the CDO keeps the class default.

#if defined(__has_include) && __has_include("ChaosVehicleMovementComponent.h")

#include "ChaosVehicleWheel.h"

#include "Dom/JsonObject.h"
#include "Engine/Blueprint.h"
#include "Misc/AutomationTest.h"
#include "Misc/Guid.h"
#include "Tests/TestUtils.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVehicleCreateWheelAssetRoundTripTest,
    "PinWright.vehicle.create_wheel_asset.RoundTrip",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVehicleCreateWheelAssetRoundTripTest::RunTest(const FString& Parameters)
{
    const FString AssetPath = FString::Printf(TEXT("/Game/__McpTest__/T_Wheel_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::DigitsLower).Left(8));

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("path"), AssetPath);
    Payload->SetNumberField(TEXT("radius"), 42.0);
    Payload->SetNumberField(TEXT("mass"), 18.0);
    Payload->SetBoolField(TEXT("bABSEnabled"), true);
    Payload->SetBoolField(TEXT("save"), false);

    FTestResponseCapture Capture;
    TestTrue(TEXT("vehicle.create_wheel_asset handler found"),
        InvokeHandlerWithCapture(TEXT("vehicle.create_wheel_asset"), Payload, Capture));
    TestTrue(TEXT("response was sent"), Capture.bWasCalled);
    TestTrue(TEXT("handler reported success"), Capture.bSuccess);
    if (!Capture.bSuccess)
    {
        AddError(FString::Printf(TEXT("create_wheel_asset failed: %s — %s"),
            *Capture.ErrorCode, *Capture.Message));
        CleanupTestAsset(AssetPath);
        return false;
    }

    UBlueprint* BP = LoadObject<UBlueprint>(nullptr, *AssetPath);
    TestNotNull(TEXT("created wheel asset loadable"), BP);
    if (!BP || !BP->GeneratedClass)
    {
        CleanupTestAsset(AssetPath);
        return false;
    }

    UChaosVehicleWheel* CDO = Cast<UChaosVehicleWheel>(BP->GeneratedClass->GetDefaultObject());
    TestNotNull(TEXT("generated CDO is a ChaosVehicleWheel"), CDO);
    if (!CDO)
    {
        CleanupTestAsset(AssetPath);
        return false;
    }

    // Counterfactual: revert the property-apply loop in the handler and these
    // three assertions fail because the CDO keeps its inherited defaults.
    TestTrue(TEXT("WheelRadius applied"),
        FMath::IsNearlyEqual(CDO->WheelRadius, 42.0f, KINDA_SMALL_NUMBER));
    TestTrue(TEXT("WheelMass applied"),
        FMath::IsNearlyEqual(CDO->WheelMass, 18.0f, KINDA_SMALL_NUMBER));
    TestTrue(TEXT("bABSEnabled applied"), CDO->bABSEnabled);

    CleanupTestAsset(AssetPath);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVehicleCreateWheelAssetBadParentClassTest,
    "PinWright.vehicle.create_wheel_asset.BadParentClass",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FVehicleCreateWheelAssetBadParentClassTest::RunTest(const FString& Parameters)
{
    const FString AssetPath = FString::Printf(TEXT("/Game/__McpTest__/T_BadWheel_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::DigitsLower).Left(8));

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("path"), AssetPath);
    Payload->SetStringField(TEXT("parentClass"), TEXT("/Script/Engine.Actor"));
    Payload->SetBoolField(TEXT("save"), false);

    FTestResponseCapture Capture;
    TestTrue(TEXT("vehicle.create_wheel_asset handler found"),
        InvokeHandlerWithCapture(TEXT("vehicle.create_wheel_asset"), Payload, Capture));
    TestTrue(TEXT("response was sent"), Capture.bWasCalled);
    TestFalse(TEXT("handler reported error for non-wheel parent class"), Capture.bSuccess);
    TestEqual(TEXT("error code is INVALID_PARENT_CLASS"),
        Capture.ErrorCode, FString(TEXT("INVALID_PARENT_CLASS")));

    // Belt-and-suspenders: ensure the asset was NOT created. The error code alone does not
    // prove the handler bailed before the factory ran — it could reject after creating the
    // package — so load the path back and require a miss. Quiet load flags keep the expected
    // lookup failure out of the automation log.
    TestNull(TEXT("rejected wheel asset was not created"),
        LoadObject<UBlueprint>(nullptr, *AssetPath, nullptr, LOAD_NoWarn | LOAD_Quiet));

    CleanupTestAsset(AssetPath);
    return true;
}

#endif // __has_include("ChaosVehicleMovementComponent.h")
