// Copyright (c) 2026 Alexander Penkin. MIT License.

// Unit tests for Character domain handlers (CharacterHandler.cpp)
// Handlers found: 27
// Tests written: 54 (one MissingRequiredParam + one ValidParamsNoCrash per handler)
#include "Misc/AutomationTest.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/HandlerRegistration.h"
#include "Handlers/ParamSpec.h"
#include "Dom/JsonObject.h"
#include "Tests/TestUtils.h"
#include "Tests/Bpir/CompilerTestUtils.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Character.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"


// ============================================================================
// character.configure_capsule_component
// Required param: "blueprintPath"
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCharacterConfigureCapsuleComponentValidParamsNoCrashTest,
    "PinWright.character.configure_capsule_component.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCharacterConfigureCapsuleComponentValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("blueprintPath"), TEXT("/Game/Characters/BP_TestChar"));
    Payload->SetNumberField(TEXT("capsuleRadius"), 42.0);
    Payload->SetNumberField(TEXT("capsuleHalfHeight"), 96.0);
    TestTrue(TEXT("configure_capsule_component handler found"),
        InvokeHandler(TEXT("character.configure_capsule_component"), Payload));
    return true;
}

// ============================================================================
// character.configure_mesh_component
// Required param: "blueprintPath"
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCharacterConfigureMeshComponentValidParamsNoCrashTest,
    "PinWright.character.configure_mesh_component.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCharacterConfigureMeshComponentValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("blueprintPath"), TEXT("/Game/Characters/BP_TestChar"));
    Payload->SetStringField(TEXT("skeletalMeshPath"), TEXT("/Game/Meshes/SK_Mannequin"));
    TestTrue(TEXT("configure_mesh_component handler found"),
        InvokeHandler(TEXT("character.configure_mesh_component"), Payload));
    return true;
}

// ============================================================================
// character.configure_camera_component
// Required param: "blueprintPath"
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCharacterConfigureCameraComponentValidParamsNoCrashTest,
    "PinWright.character.configure_camera_component.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCharacterConfigureCameraComponentValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("blueprintPath"), TEXT("/Game/Characters/BP_TestChar"));
    Payload->SetNumberField(TEXT("springArmLength"), 300.0);
    Payload->SetBoolField(TEXT("cameraUsePawnControlRotation"), true);
    TestTrue(TEXT("configure_camera_component handler found"),
        InvokeHandler(TEXT("character.configure_camera_component"), Payload));
    return true;
}

// ============================================================================
// character.configure_movement_speeds
// Required param: "blueprintPath"
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCharacterConfigureMovementSpeedsValidParamsNoCrashTest,
    "PinWright.character.configure_movement_speeds.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCharacterConfigureMovementSpeedsValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    // Pass a single ground speed: walkSpeed and runSpeed are aliases of the same
    // MaxWalkSpeed field, so passing both is now rejected (see the
    // WalkRunSpeedAliasRejectsBothAndEchoes regression test below). This smoke
    // test exercises the normal one-speed success path.
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("blueprintPath"), TEXT("/Game/Characters/BP_TestChar"));
    Payload->SetNumberField(TEXT("walkSpeed"), 600.0);
    Payload->SetNumberField(TEXT("crouchSpeed"), 300.0);
    TestTrue(TEXT("configure_movement_speeds handler found"),
        InvokeHandler(TEXT("character.configure_movement_speeds"), Payload));
    return true;
}

// ============================================================================
// character.configure_jump
// Required param: "blueprintPath"
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCharacterConfigureJumpValidParamsNoCrashTest,
    "PinWright.character.configure_jump.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCharacterConfigureJumpValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("blueprintPath"), TEXT("/Game/Characters/BP_TestChar"));
    Payload->SetNumberField(TEXT("jumpHeight"), 600.0);
    Payload->SetNumberField(TEXT("airControl"), 0.35);
    Payload->SetNumberField(TEXT("maxJumpCount"), 2.0);
    TestTrue(TEXT("configure_jump handler found"),
        InvokeHandler(TEXT("character.configure_jump"), Payload));
    return true;
}

// ============================================================================
// character.configure_rotation
// Required param: "blueprintPath"
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCharacterConfigureRotationValidParamsNoCrashTest,
    "PinWright.character.configure_rotation.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCharacterConfigureRotationValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("blueprintPath"), TEXT("/Game/Characters/BP_TestChar"));
    Payload->SetBoolField(TEXT("orientToMovement"), true);
    Payload->SetBoolField(TEXT("useControllerRotationYaw"), false);
    Payload->SetNumberField(TEXT("rotationRate"), 540.0);
    TestTrue(TEXT("configure_rotation handler found"),
        InvokeHandler(TEXT("character.configure_rotation"), Payload));
    return true;
}

// ============================================================================
// character.add_custom_movement_mode
// Required param: "blueprintPath"
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCharacterAddCustomMovementModeValidParamsNoCrashTest,
    "PinWright.character.add_custom_movement_mode.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCharacterAddCustomMovementModeValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("blueprintPath"), TEXT("/Game/Characters/BP_TestChar"));
    Payload->SetStringField(TEXT("modeName"), TEXT("Gliding"));
    Payload->SetNumberField(TEXT("modeId"), 1.0);
    Payload->SetNumberField(TEXT("customSpeed"), 500.0);
    TestTrue(TEXT("add_custom_movement_mode handler found"),
        InvokeHandler(TEXT("character.add_custom_movement_mode"), Payload));
    return true;
}

// ============================================================================
// character.configure_nav_movement
// Required param: "blueprintPath"
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCharacterConfigureNavMovementValidParamsNoCrashTest,
    "PinWright.character.configure_nav_movement.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCharacterConfigureNavMovementValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("blueprintPath"), TEXT("/Game/Characters/BP_TestChar"));
    Payload->SetNumberField(TEXT("navAgentRadius"), 42.0);
    Payload->SetNumberField(TEXT("navAgentHeight"), 192.0);
    Payload->SetBoolField(TEXT("avoidanceEnabled"), true);
    TestTrue(TEXT("configure_nav_movement handler found"),
        InvokeHandler(TEXT("character.configure_nav_movement"), Payload));
    return true;
}
// ============================================================================
// character.map_surface_to_sound
// Required param: "blueprintPath"
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCharacterMapSurfaceToSoundValidParamsNoCrashTest,
    "PinWright.character.map_surface_to_sound.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCharacterMapSurfaceToSoundValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("blueprintPath"), TEXT("/Game/Characters/BP_TestChar"));
    Payload->SetStringField(TEXT("surfaceType"), TEXT("Concrete"));
    Payload->SetStringField(TEXT("footstepSoundPath"), TEXT("/Game/Audio/Footsteps/S_Concrete"));
    TestTrue(TEXT("map_surface_to_sound handler found"),
        InvokeHandler(TEXT("character.map_surface_to_sound"), Payload));
    return true;
}

// ============================================================================
// character.configure_footstep_fx
// Required param: "blueprintPath"
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCharacterConfigureFootstepFxValidParamsNoCrashTest,
    "PinWright.character.configure_footstep_fx.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCharacterConfigureFootstepFxValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("blueprintPath"), TEXT("/Game/Characters/BP_TestChar"));
    Payload->SetNumberField(TEXT("volumeMultiplier"), 1.0);
    Payload->SetNumberField(TEXT("particleScale"), 1.0);
    TestTrue(TEXT("configure_footstep_fx handler found"),
        InvokeHandler(TEXT("character.configure_footstep_fx"), Payload));
    return true;
}

// ============================================================================
// Regression: footstep/movement verbs must PERSIST variable defaults, not just
// echo their inputs. Guards B-map-surface-to-sound-no-effect — before the fix,
// SetBPVarDefaultValue was a no-op stub and map_surface_to_sound never inserted
// the surface->sound entry, so every NewVariables[].DefaultValue stayed empty.
// This drives the real handlers against an in-memory ACharacter blueprint and
// asserts the defaults are actually written; it fails if the fix is reverted.
// ============================================================================

namespace
{
    // Returns the persisted DefaultValue string of a member variable, or empty
    // if the variable isn't present. Mirrors what blueprint.get reports under
    // "defaults" and what the handlers now write via SetBPVarDefaultValue.
    // Thin wrapper over the shared NewVariables-by-name lookup so the case /
    // null handling stays in one place (CompilerTestUtils::FindNewVariableByName).
    FString FindCharVarDefault(const UBlueprint* BP, const FString& VarName)
    {
        const FBPVariableDescription* Var = CompilerTestUtils::FindNewVariableByName(BP, VarName);
        return Var ? Var->DefaultValue : FString();
    }

    // Creates a real, persisted ACharacter blueprint the path-based handlers can
    // LoadObject by path (CompilerTestUtils only makes transient BPs, which these
    // handlers can't resolve). Builds a GUID-unique /Game/__PW_GatewayTests/<Slug>_<guid>
    // package, asserts package + blueprint creation, and returns the BP (nullptr on
    // failure). Writes the package path to OutPackagePath; the caller owns
    // CleanupTestAsset(OutPackagePath).
    UBlueprint* CreatePersistedCharacterBP(FAutomationTestBase& Test, const FString& Slug, FString& OutPackagePath)
    {
        OutPackagePath = FString::Printf(
            TEXT("/Game/__PW_GatewayTests/%s_%s"),
            *Slug, *FGuid::NewGuid().ToString(EGuidFormats::Digits));
        UPackage* Pkg = CreatePackage(*OutPackagePath);
        Test.TestNotNull(TEXT("package created"), Pkg);
        if (!Pkg) return nullptr;

        UBlueprint* BP = FKismetEditorUtilities::CreateBlueprint(
            ACharacter::StaticClass(), Pkg,
            FName(*FPackageName::GetLongPackageAssetName(OutPackagePath)),
            BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
        Test.TestNotNull(TEXT("character blueprint created"), BP);
        return BP;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCharacterFootstepDefaultsPersistedTest,
    "PinWright.character.footstep_defaults.Persisted",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCharacterFootstepDefaultsPersistedTest::RunTest(const FString& Parameters)
{
    // Create a real ACharacter blueprint the handlers can LoadObject by path.
    FString PackagePath;
    UBlueprint* BP = CreatePersistedCharacterBP(*this, TEXT("FootstepDefaults"), PackagePath);
    if (!BP) { CleanupTestAsset(PackagePath); return true; }

    // 1. configure_footstep_fx must write the two float defaults (was dropped).
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("blueprintPath"), PackagePath);
        Payload->SetNumberField(TEXT("volumeMultiplier"), 0.8);
        Payload->SetNumberField(TEXT("particleScale"), 1.25);
        TestTrue(TEXT("configure_footstep_fx handler found"),
            InvokeHandler(TEXT("character.configure_footstep_fx"), Payload));
    }
    const FString VolDefault = FindCharVarDefault(BP, TEXT("FootstepVolumeMultiplier"));
    const FString ScaleDefault = FindCharVarDefault(BP, TEXT("FootstepParticleScale"));
    TestTrue(TEXT("FootstepVolumeMultiplier default is stored (not empty)"), !VolDefault.IsEmpty());
    TestTrue(TEXT("FootstepParticleScale default is stored (not empty)"), !ScaleDefault.IsEmpty());
    TestTrue(TEXT("FootstepVolumeMultiplier default carries 0.8"), VolDefault.Contains(TEXT("0.8")));
    TestTrue(TEXT("FootstepParticleScale default carries 1.25"), ScaleDefault.Contains(TEXT("1.25")));

    // 2. map_surface_to_sound must accumulate distinct surface->sound entries
    //    into the FootstepSoundMap default (was a silent no-op echoing inputs).
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("blueprintPath"), PackagePath);
        Payload->SetStringField(TEXT("surfaceType"), TEXT("SurfaceType_Default"));
        Payload->SetStringField(TEXT("footstepSoundPath"), TEXT("/Game/Audio/Footsteps/S_Wood01"));
        TestTrue(TEXT("map_surface_to_sound handler found (1)"),
            InvokeHandler(TEXT("character.map_surface_to_sound"), Payload));
    }
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("blueprintPath"), PackagePath);
        Payload->SetStringField(TEXT("surfaceType"), TEXT("SurfaceType1"));
        Payload->SetStringField(TEXT("footstepSoundPath"), TEXT("/Game/Audio/Footsteps/S_Glass01"));
        TestTrue(TEXT("map_surface_to_sound handler found (2)"),
            InvokeHandler(TEXT("character.map_surface_to_sound"), Payload));
    }

    const FString MapDefault = FindCharVarDefault(BP, TEXT("FootstepSoundMap"));
    TestTrue(TEXT("FootstepSoundMap default is stored (not empty/just '()')"),
        !MapDefault.IsEmpty() && MapDefault != TEXT("()"));
    // Both surface keys and both sound paths must survive accumulation.
    TestTrue(TEXT("map default contains SurfaceType_Default key"),
        MapDefault.Contains(TEXT("SurfaceType_Default")));
    TestTrue(TEXT("map default contains SurfaceType1 key"),
        MapDefault.Contains(TEXT("SurfaceType1")));
    TestTrue(TEXT("map default contains first sound path"),
        MapDefault.Contains(TEXT("S_Wood01")));
    TestTrue(TEXT("map default contains second sound path"),
        MapDefault.Contains(TEXT("S_Glass01")));

    CleanupTestAsset(PackagePath);
    return true;
}

// ============================================================================
// Regression: configure_movement_speeds walkSpeed/runSpeed alias collision.
// Guards E-character-walk-run-speed-alias. walkSpeed and runSpeed are aliases
// for the single MaxWalkSpeed field; passing both used to silently drop
// walkSpeed (runSpeed applied second clobbered it) with no in-band signal, and
// no applied speed was echoed in the result. The fix (a) rejects the ambiguous
// both-present case with INVALID_PARAMS before mutating, and (b) echoes the
// applied walkSpeed (== resulting MaxWalkSpeed) on the single-param success
// path. This drives the real handler against an in-memory ACharacter blueprint;
// it fails if either the both-present guard or the result echo is reverted.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCharacterConfigureMovementSpeedsWalkRunAliasTest,
    "PinWright.character.configure_movement_speeds.WalkRunSpeedAliasRejectsBothAndEchoes",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCharacterConfigureMovementSpeedsWalkRunAliasTest::RunTest(const FString& Parameters)
{
    FString PackagePath;
    UBlueprint* BP = CreatePersistedCharacterBP(*this, TEXT("WalkRunAlias"), PackagePath);
    if (!BP) { CleanupTestAsset(PackagePath); return true; }

    // 1. Passing BOTH walkSpeed and runSpeed (aliases of MaxWalkSpeed) is
    //    ambiguous and must be rejected with INVALID_PARAMS rather than silently
    //    dropping walkSpeed. This is the core of the alias-collision defect.
    {
        FTestResponseCapture Capture;
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("blueprintPath"), PackagePath);
        Payload->SetNumberField(TEXT("walkSpeed"), 250.0);
        Payload->SetNumberField(TEXT("runSpeed"), 650.0);
        TestTrue(TEXT("configure_movement_speeds handler found (both)"),
            InvokeHandlerWithCapture(TEXT("character.configure_movement_speeds"), Payload, Capture));
        TestFalse(TEXT("both walkSpeed+runSpeed is rejected, not a silent success"),
            Capture.bSuccess);
        TestEqual(TEXT("both-present rejection uses INVALID_PARAMS"),
            Capture.ErrorCode, FString(TEXT("INVALID_PARAMS")));
    }

    // 2. A single walkSpeed succeeds and the applied speed is echoed in-band
    //    (previously the result echoed no speed at all).
    {
        FTestResponseCapture Capture;
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("blueprintPath"), PackagePath);
        Payload->SetNumberField(TEXT("walkSpeed"), 250.0);
        TestTrue(TEXT("configure_movement_speeds handler found (walkSpeed only)"),
            InvokeHandlerWithCapture(TEXT("character.configure_movement_speeds"), Payload, Capture));
        TestTrue(TEXT("walkSpeed-only call succeeds"), Capture.bSuccess);
        if (Capture.Result.IsValid())
        {
            double Echoed = 0.0;
            const bool bHasWalk = Capture.Result->TryGetNumberField(TEXT("walkSpeed"), Echoed);
            TestTrue(TEXT("result echoes applied walkSpeed"), bHasWalk);
            TestEqual(TEXT("echoed walkSpeed is the applied 250"), Echoed, 250.0);
        }
        else
        {
            AddError(TEXT("walkSpeed-only call returned no result object"));
        }
    }

    // 3. A single runSpeed succeeds and is echoed under walkSpeed (proving it
    //    targets the same MaxWalkSpeed field, the documented mapping).
    {
        FTestResponseCapture Capture;
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("blueprintPath"), PackagePath);
        Payload->SetNumberField(TEXT("runSpeed"), 650.0);
        TestTrue(TEXT("configure_movement_speeds handler found (runSpeed only)"),
            InvokeHandlerWithCapture(TEXT("character.configure_movement_speeds"), Payload, Capture));
        TestTrue(TEXT("runSpeed-only call succeeds"), Capture.bSuccess);
        if (Capture.Result.IsValid())
        {
            double Echoed = 0.0;
            const bool bHasWalk = Capture.Result->TryGetNumberField(TEXT("walkSpeed"), Echoed);
            TestTrue(TEXT("result echoes applied runSpeed under walkSpeed"), bHasWalk);
            TestEqual(TEXT("echoed MaxWalkSpeed is the applied runSpeed 650"), Echoed, 650.0);
        }
        else
        {
            AddError(TEXT("runSpeed-only call returned no result object"));
        }
    }

    CleanupTestAsset(PackagePath);
    return true;
}

// ============================================================================
// character.get_character_info
// Required param: "blueprintPath"
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCharacterGetCharacterInfoValidParamsNoCrashTest,
    "PinWright.character.get_character_info.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCharacterGetCharacterInfoValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("blueprintPath"), TEXT("/Game/Characters/BP_TestChar"));
    TestTrue(TEXT("get_character_info handler found"),
        InvokeHandler(TEXT("character.get_character_info"), Payload));
    return true;
}

// ============================================================================
// Regression: get_character_info must report groundFriction and
// brakingDeceleration so configure_movement_speeds writes can be read
// back. Guards F-character-info-no-friction-braking — before the fix the
// getter emitted walkSpeed/jumpZVelocity/.../gravityScale but neither
// friction field, so a "set then read-back-confirm" task could not be
// satisfied through the mandated reader. This drives configure_movement_speeds
// and the getter against an in-memory ACharacter blueprint and asserts the
// two values round-trip; it fails if the getter's two SetNumberField lines
// are reverted.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCharacterGetCharacterInfoFrictionBrakingRoundTripTest,
    "PinWright.character.get_character_info.FrictionBrakingRoundTrip",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCharacterGetCharacterInfoFrictionBrakingRoundTripTest::RunTest(const FString& Parameters)
{
    // Create a real ACharacter blueprint the handlers can LoadObject by path.
    FString PackagePath;
    UBlueprint* BP = CreatePersistedCharacterBP(*this, TEXT("FrictionBraking"), PackagePath);
    if (!BP) { CleanupTestAsset(PackagePath); return true; }

    // Seed both fields through configure_movement_speeds (the write surface).
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("blueprintPath"), PackagePath);
        Payload->SetNumberField(TEXT("groundFriction"), 12.5);
        TestTrue(TEXT("configure_movement_speeds handler found"),
            InvokeHandler(TEXT("character.configure_movement_speeds"), Payload));
    }
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("blueprintPath"), PackagePath);
        Payload->SetNumberField(TEXT("deceleration"), 2600.0);
        TestTrue(TEXT("configure_movement_speeds handler found"),
            InvokeHandler(TEXT("character.configure_movement_speeds"), Payload));
    }

    // Read back through the mandated getter and confirm both values survive.
    FTestResponseCapture Capture;
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("blueprintPath"), PackagePath);
    TestTrue(TEXT("get_character_info handler found"),
        InvokeHandlerWithCapture(TEXT("character.get_character_info"), Payload, Capture));
    TestTrue(TEXT("get_character_info succeeded"), Capture.bSuccess);

    if (Capture.Result.IsValid())
    {
        double GroundFriction = 0.0;
        const bool bHasFriction = Capture.Result->TryGetNumberField(TEXT("groundFriction"), GroundFriction);
        TestTrue(TEXT("get_character_info reports groundFriction"), bHasFriction);
        TestEqual(TEXT("groundFriction round-trips 12.5"), GroundFriction, 12.5);

        double BrakingDeceleration = 0.0;
        const bool bHasBraking = Capture.Result->TryGetNumberField(TEXT("brakingDeceleration"), BrakingDeceleration);
        TestTrue(TEXT("get_character_info reports brakingDeceleration"), bHasBraking);
        TestEqual(TEXT("brakingDeceleration round-trips 2600"), BrakingDeceleration, 2600.0);
    }
    else
    {
        AddError(TEXT("get_character_info returned no result object"));
    }

    CleanupTestAsset(PackagePath);
    return true;
}

// ============================================================================
// Regression: get_character_info must report the configure_nav_movement
// nav-agent fields (navAgentRadius/navAgentHeight/avoidanceEnabled) so they can
// be read back through the mandated reader instead of a cross-namespace
// property.get on the CharacterMovement CDO. Guards
// E-character-readback-fallback-undocumented — the deep fix mirrors the
// F-character-info-no-friction-braking precedent (extend the reader, not the
// docs). It drives configure_nav_movement then the getter against an in-memory
// ACharacter blueprint and asserts the three values round-trip; it fails if the
// getter's three nav-agent SetNumberField/SetBoolField lines are reverted.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCharacterGetCharacterInfoNavAgentRoundTripTest,
    "PinWright.character.get_character_info.NavAgentRoundTrip",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCharacterGetCharacterInfoNavAgentRoundTripTest::RunTest(const FString& Parameters)
{
    FString PackagePath;
    UBlueprint* BP = CreatePersistedCharacterBP(*this, TEXT("NavAgent"), PackagePath);
    if (!BP) { CleanupTestAsset(PackagePath); return true; }

    // Seed all three nav-agent fields through configure_nav_movement (the write
    // surface), using non-default values so the round-trip is unambiguous.
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("blueprintPath"), PackagePath);
        Payload->SetNumberField(TEXT("navAgentRadius"), 55.0);
        Payload->SetNumberField(TEXT("navAgentHeight"), 210.0);
        Payload->SetBoolField(TEXT("avoidanceEnabled"), true);
        TestTrue(TEXT("configure_nav_movement handler found"),
            InvokeHandler(TEXT("character.configure_nav_movement"), Payload));
    }

    // Read back through the mandated getter and confirm all three values survive.
    FTestResponseCapture Capture;
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("blueprintPath"), PackagePath);
    TestTrue(TEXT("get_character_info handler found"),
        InvokeHandlerWithCapture(TEXT("character.get_character_info"), Payload, Capture));
    TestTrue(TEXT("get_character_info succeeded"), Capture.bSuccess);

    if (Capture.Result.IsValid())
    {
        double NavAgentRadius = 0.0;
        const bool bHasRadius = Capture.Result->TryGetNumberField(TEXT("navAgentRadius"), NavAgentRadius);
        TestTrue(TEXT("get_character_info reports navAgentRadius"), bHasRadius);
        TestEqual(TEXT("navAgentRadius round-trips 55"), NavAgentRadius, 55.0);

        double NavAgentHeight = 0.0;
        const bool bHasHeight = Capture.Result->TryGetNumberField(TEXT("navAgentHeight"), NavAgentHeight);
        TestTrue(TEXT("get_character_info reports navAgentHeight"), bHasHeight);
        TestEqual(TEXT("navAgentHeight round-trips 210"), NavAgentHeight, 210.0);

        bool bAvoidance = false;
        const bool bHasAvoidance = Capture.Result->TryGetBoolField(TEXT("avoidanceEnabled"), bAvoidance);
        TestTrue(TEXT("get_character_info reports avoidanceEnabled"), bHasAvoidance);
        TestTrue(TEXT("avoidanceEnabled round-trips true"), bAvoidance);
    }
    else
    {
        AddError(TEXT("get_character_info returned no result object"));
    }

    CleanupTestAsset(PackagePath);
    return true;
}
// ============================================================================
// character.configure_crouch
// Required param: "blueprintPath"
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCharacterConfigureCrouchValidParamsNoCrashTest,
    "PinWright.character.configure_crouch.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCharacterConfigureCrouchValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("blueprintPath"), TEXT("/Game/Characters/BP_TestChar"));
    Payload->SetNumberField(TEXT("crouchSpeed"), 300.0);
    Payload->SetNumberField(TEXT("crouchedHalfHeight"), 44.0);
    Payload->SetBoolField(TEXT("canCrouch"), true);
    TestTrue(TEXT("configure_crouch handler found"),
        InvokeHandler(TEXT("character.configure_crouch"), Payload));
    return true;
}

// ============================================================================
// character.configure_sprint
// Required param: "blueprintPath"
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCharacterConfigureSprintValidParamsNoCrashTest,
    "PinWright.character.configure_sprint.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCharacterConfigureSprintValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("blueprintPath"), TEXT("/Game/Characters/BP_TestChar"));
    Payload->SetNumberField(TEXT("sprintSpeed"), 900.0);
    TestTrue(TEXT("configure_sprint handler found"),
        InvokeHandler(TEXT("character.configure_sprint"), Payload));
    return true;
}
