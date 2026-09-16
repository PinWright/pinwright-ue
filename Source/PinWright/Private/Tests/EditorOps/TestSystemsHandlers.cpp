// Copyright (c) 2026 Alexander Penkin. MIT License.

// Unit tests for Systems domain handlers: GameFramework, GAS.
// Each handler with required params gets a MissingRequiredParam test and a ValidParamsNoCrash test.
// Handlers with no required params get a single ValidParamsNoCrash test.
#include "Misc/AutomationTest.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/HandlerRegistration.h"
#include "Handlers/ParamSpec.h"
#include "Dom/JsonObject.h"
#include "GameplayTagsManager.h"
#include "GameplayTagsEditorModule.h"  // AddTransientEditorGameplayTag — register the test tag for this session
#include "Misc/ScopeExit.h"
#include "Tests/TestUtils.h"
#include "Tests/TestSkipReporting.h"

namespace
{
    // Drives a gas.create_* handler with a unique GUID-suffixed name under the shared
    // test folder, then force-deletes the created asset. A fixed root-level fixture
    // name (e.g. /Game/EC_DamageCalc) collides with a stale .uasset leaked by a prior
    // suite run: cleanup's DeleteAsset then cold-loads the stale file into the package
    // occupied by the freshly created blueprint and crashes in
    // FLinkerLoad::RegenerateBlueprintClass (garbage FFieldPath access violation).
    void RunGASCreateHandlerAtUniquePath(FAutomationTestBase& Test, const TCHAR* Method,
        const TCHAR* NamePrefix)
    {
        const FString Name = FString::Printf(TEXT("%s_%s"), NamePrefix,
            *FGuid::NewGuid().ToString(EGuidFormats::Digits));
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("name"), Name);
        Payload->SetStringField(TEXT("path"), TEXT("/Game/__PW_GatewayTests"));
        Test.TestTrue(TEXT("Handler found"), InvokeHandler(Method, Payload));
        CleanupTestAsset(FString::Printf(TEXT("/Game/__PW_GatewayTests/%s"), *Name));
    }
}

// ============================================================================
// GAME FRAMEWORK — game_framework.configure_game_rules  (REQ: gameModeBlueprint)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGameFrameworkConfigureGameRulesValidParamsNoCrashTest,
    "PinWright.game_framework.configure_game_rules.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGameFrameworkConfigureGameRulesValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("gameModeBlueprint"), TEXT("/Game/BP_TestGameMode"));
    Payload->SetNumberField(TEXT("maxPlayers"), 8.0);
    TestTrue(TEXT("Handler found"), InvokeHandler(TEXT("game_framework.configure_game_rules"), Payload));
    return true;
}
// ============================================================================
// GAME FRAMEWORK — game_framework.configure_round_system  (REQ: gameModeBlueprint)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGameFrameworkConfigureRoundSystemValidParamsNoCrashTest,
    "PinWright.game_framework.configure_round_system.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGameFrameworkConfigureRoundSystemValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("gameModeBlueprint"), TEXT("/Game/BP_TestGameMode"));
    Payload->SetNumberField(TEXT("maxRounds"), 5.0);
    TestTrue(TEXT("Handler found"), InvokeHandler(TEXT("game_framework.configure_round_system"), Payload));
    return true;
}

// ============================================================================
// GAME FRAMEWORK — game_framework.configure_team_system  (REQ: gameModeBlueprint)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGameFrameworkConfigureTeamSystemValidParamsNoCrashTest,
    "PinWright.game_framework.configure_team_system.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGameFrameworkConfigureTeamSystemValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("gameModeBlueprint"), TEXT("/Game/BP_TestGameMode"));
    Payload->SetNumberField(TEXT("maxTeams"), 2.0);
    TestTrue(TEXT("Handler found"), InvokeHandler(TEXT("game_framework.configure_team_system"), Payload));
    return true;
}

// ============================================================================
// GAME FRAMEWORK — game_framework.configure_scoring_system  (REQ: gameModeBlueprint)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGameFrameworkConfigureScoringSystemValidParamsNoCrashTest,
    "PinWright.game_framework.configure_scoring_system.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGameFrameworkConfigureScoringSystemValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("gameModeBlueprint"), TEXT("/Game/BP_TestGameMode"));
    Payload->SetNumberField(TEXT("scoreToWin"), 100.0);
    TestTrue(TEXT("Handler found"), InvokeHandler(TEXT("game_framework.configure_scoring_system"), Payload));
    return true;
}

// ============================================================================
// GAME FRAMEWORK — game_framework.configure_spawn_system  (REQ: gameModeBlueprint)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGameFrameworkConfigureSpawnSystemValidParamsNoCrashTest,
    "PinWright.game_framework.configure_spawn_system.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGameFrameworkConfigureSpawnSystemValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("gameModeBlueprint"), TEXT("/Game/BP_TestGameMode"));
    Payload->SetStringField(TEXT("spawnMethod"), TEXT("Random"));
    TestTrue(TEXT("Handler found"), InvokeHandler(TEXT("game_framework.configure_spawn_system"), Payload));
    return true;
}

// ============================================================================
// GAME FRAMEWORK — game_framework.configure_player_start  (no required params)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGameFrameworkConfigurePlayerStartValidParamsNoCrashTest,
    "PinWright.game_framework.configure_player_start.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGameFrameworkConfigurePlayerStartValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TestTrue(TEXT("Handler found"), InvokeHandler(TEXT("game_framework.configure_player_start"), MakeShared<FJsonObject>()));
    return true;
}

// ============================================================================
// GAME FRAMEWORK — game_framework.set_respawn_rules  (REQ: gameModeBlueprint)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGameFrameworkSetRespawnRulesValidParamsNoCrashTest,
    "PinWright.game_framework.set_respawn_rules.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGameFrameworkSetRespawnRulesValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("gameModeBlueprint"), TEXT("/Game/BP_TestGameMode"));
    Payload->SetNumberField(TEXT("respawnDelay"), 3.0);
    TestTrue(TEXT("Handler found"), InvokeHandler(TEXT("game_framework.set_respawn_rules"), Payload));
    return true;
}

// ============================================================================
// GAME FRAMEWORK — game_framework.configure_spectating  (REQ: gameModeBlueprint)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGameFrameworkConfigureSpectatingValidParamsNoCrashTest,
    "PinWright.game_framework.configure_spectating.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGameFrameworkConfigureSpectatingValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("gameModeBlueprint"), TEXT("/Game/BP_TestGameMode"));
    Payload->SetBoolField(TEXT("bAllowSpectating"), true);
    TestTrue(TEXT("Handler found"), InvokeHandler(TEXT("game_framework.configure_spectating"), Payload));
    return true;
}

// ============================================================================
// GAME FRAMEWORK — game_framework.get_game_framework_info  (no required params)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGameFrameworkGetGameFrameworkInfoValidParamsNoCrashTest,
    "PinWright.game_framework.get_game_framework_info.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGameFrameworkGetGameFrameworkInfoValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TestTrue(TEXT("Handler found"), InvokeHandler(TEXT("game_framework.get_game_framework_info"), MakeShared<FJsonObject>()));
    return true;
}

// ============================================================================
// GAS — gas.create_attribute_set  (REQ: name)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGASCreateAttributeSetValidParamsNoCrashTest,
    "PinWright.gas.create_attribute_set.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGASCreateAttributeSetValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    RunGASCreateHandlerAtUniquePath(*this, TEXT("gas.create_attribute_set"), TEXT("AS_Health"));
    return true;
}

// ============================================================================
// GAS — gas.add_attribute  (REQ: blueprintPath, attributeName)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGASAddAttributeValidParamsNoCrashTest,
    "PinWright.gas.add_attribute.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGASAddAttributeValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("blueprintPath"), TEXT("/Game/GAS/AS_Health"));
    Payload->SetStringField(TEXT("attributeName"), TEXT("Health"));
    TestTrue(TEXT("Handler found"), InvokeHandler(TEXT("gas.add_attribute"), Payload));
    return true;
}

// ============================================================================
// GAS — gas.set_attribute_base_value  (REQ: blueprintPath, attributeName)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGASSetAttributeBaseValueValidParamsNoCrashTest,
    "PinWright.gas.set_attribute_base_value.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGASSetAttributeBaseValueValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("blueprintPath"), TEXT("/Game/GAS/AS_Health"));
    Payload->SetStringField(TEXT("attributeName"), TEXT("Health"));
    Payload->SetNumberField(TEXT("baseValue"), 100.0);
    TestTrue(TEXT("Handler found"), InvokeHandler(TEXT("gas.set_attribute_base_value"), Payload));
    return true;
}

// ============================================================================
// GAS — gas.create_gameplay_ability  (REQ: name)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGASCreateGameplayAbilityValidParamsNoCrashTest,
    "PinWright.gas.create_gameplay_ability.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGASCreateGameplayAbilityValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    RunGASCreateHandlerAtUniquePath(*this, TEXT("gas.create_gameplay_ability"), TEXT("GA_FireBolt"));
    return true;
}

// ============================================================================
// GAS — gas.set_ability_tags  (REQ: blueprintPath)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGASSetAbilityTagsValidParamsNoCrashTest,
    "PinWright.gas.set_ability_tags.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGASSetAbilityTagsValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("blueprintPath"), TEXT("/Game/GAS/GA_FireBolt"));
    TestTrue(TEXT("Handler found"), InvokeHandler(TEXT("gas.set_ability_tags"), Payload));
    return true;
}

// ============================================================================
// GAS — gas.set_ability_costs  (REQ: blueprintPath)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGASSetAbilityCostsValidParamsNoCrashTest,
    "PinWright.gas.set_ability_costs.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGASSetAbilityCostsValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("blueprintPath"), TEXT("/Game/GAS/GA_FireBolt"));
    TestTrue(TEXT("Handler found"), InvokeHandler(TEXT("gas.set_ability_costs"), Payload));
    return true;
}

// ============================================================================
// GAS — gas.set_ability_cooldown  (REQ: blueprintPath)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGASSetAbilityCooldownValidParamsNoCrashTest,
    "PinWright.gas.set_ability_cooldown.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGASSetAbilityCooldownValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("blueprintPath"), TEXT("/Game/GAS/GA_FireBolt"));
    TestTrue(TEXT("Handler found"), InvokeHandler(TEXT("gas.set_ability_cooldown"), Payload));
    return true;
}

// ============================================================================
// GAS — gas.set_activation_policy  (REQ: blueprintPath)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGASSetActivationPolicyValidParamsNoCrashTest,
    "PinWright.gas.set_activation_policy.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGASSetActivationPolicyValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("blueprintPath"), TEXT("/Game/GAS/GA_FireBolt"));
    TestTrue(TEXT("Handler found"), InvokeHandler(TEXT("gas.set_activation_policy"), Payload));
    return true;
}

// ============================================================================
// GAS — gas.set_instancing_policy  (REQ: blueprintPath)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGASSetInstancingPolicyValidParamsNoCrashTest,
    "PinWright.gas.set_instancing_policy.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGASSetInstancingPolicyValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("blueprintPath"), TEXT("/Game/GAS/GA_FireBolt"));
    TestTrue(TEXT("Handler found"), InvokeHandler(TEXT("gas.set_instancing_policy"), Payload));
    return true;
}

// ============================================================================
// GAS — gas.create_gameplay_effect  (REQ: name)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGASCreateGameplayEffectValidParamsNoCrashTest,
    "PinWright.gas.create_gameplay_effect.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGASCreateGameplayEffectValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    RunGASCreateHandlerAtUniquePath(*this, TEXT("gas.create_gameplay_effect"), TEXT("GE_Burn"));
    return true;
}

// ============================================================================
// GAS — gas.set_effect_duration  (REQ: blueprintPath)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGASSetEffectDurationValidParamsNoCrashTest,
    "PinWright.gas.set_effect_duration.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGASSetEffectDurationValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("blueprintPath"), TEXT("/Game/GAS/GE_Burn"));
    Payload->SetStringField(TEXT("durationType"), TEXT("has_duration"));
    Payload->SetNumberField(TEXT("duration"), 5.0);
    TestTrue(TEXT("Handler found"), InvokeHandler(TEXT("gas.set_effect_duration"), Payload));
    return true;
}

// ============================================================================
// GAS — gas.add_effect_modifier  (REQ: blueprintPath)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGASAddEffectModifierValidParamsNoCrashTest,
    "PinWright.gas.add_effect_modifier.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGASAddEffectModifierValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("blueprintPath"), TEXT("/Game/GAS/GE_Burn"));
    Payload->SetStringField(TEXT("operation"), TEXT("additive"));
    Payload->SetNumberField(TEXT("magnitude"), -10.0);
    TestTrue(TEXT("Handler found"), InvokeHandler(TEXT("gas.add_effect_modifier"), Payload));
    return true;
}

// Registration-metadata regression for F-gas-modifier-no-attribute-binding:
// gas.add_effect_modifier must expose the optional 'attribute' param (with the
// 'attributeName' alias) so a modifier can be bound to the attribute it modifies.
// Reverting the param spec drops 'attribute' from the registration and the
// dispatcher then rejects it with UNKNOWN_PARAMS — exactly the gap the ticket
// reports — so this test (which reads the live registration the dispatcher uses)
// fails. Lighter sibling to the behavioral BindsAttribute test, and GAS-agnostic
// so it runs even where the GameplayAbilities plugin is absent.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGASAddEffectModifierExposesAttributeParamTest,
    "PinWright.gas.add_effect_modifier.ExposesAttributeParam",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGASAddEffectModifierExposesAttributeParamTest::RunTest(const FString& Parameters)
{
    bool bFoundHandler = false;
    bool bHasAttributeParam = false;
    bool bHasAttributeNameAlias = false;
    for (const FHandlerRegistration& Reg : FAutoRegisterHandler::GetPendingRegistrations())
    {
        if (Reg.MethodName != TEXT("gas.add_effect_modifier"))
        {
            continue;
        }
        bFoundHandler = true;
        for (const FParamSpec& Spec : Reg.Params)
        {
            if (Spec.Name == TEXT("attribute"))
            {
                bHasAttributeParam = true;
                bHasAttributeNameAlias = Spec.Aliases.Contains(TEXT("attributeName"));
            }
        }
    }
    TestTrue(TEXT("gas.add_effect_modifier is registered"), bFoundHandler);
    TestTrue(TEXT("gas.add_effect_modifier exposes the 'attribute' param"), bHasAttributeParam);
    TestTrue(TEXT("'attribute' carries the 'attributeName' alias"), bHasAttributeNameAlias);
    return true;
}

// Registration regression for the paired write handler from
// F-gas-modifier-no-attribute-binding: gas.set_modifier_attribute must exist so
// an existing modifier's attribute can be (re)bound. Removing the handler fails
// this test. GAS-agnostic (registration only).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGASSetModifierAttributeRegisteredTest,
    "PinWright.gas.set_modifier_attribute.Registered",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGASSetModifierAttributeRegisteredTest::RunTest(const FString& Parameters)
{
    TestTrue(TEXT("gas.set_modifier_attribute is registered"),
        IsHandlerRegistered(TEXT("gas.set_modifier_attribute")));
    // A missing required 'attribute'/'blueprintPath' must not crash the handler.
    TestTrue(TEXT("Handler found"),
        InvokeHandler(TEXT("gas.set_modifier_attribute"), MakeShared<FJsonObject>()));
    return true;
}

// ============================================================================
// GAS — gas.set_modifier_magnitude  (REQ: blueprintPath)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGASSetModifierMagnitudeValidParamsNoCrashTest,
    "PinWright.gas.set_modifier_magnitude.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGASSetModifierMagnitudeValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("blueprintPath"), TEXT("/Game/GAS/GE_Burn"));
    Payload->SetNumberField(TEXT("modifierIndex"), 0.0);
    Payload->SetNumberField(TEXT("value"), -5.0);
    TestTrue(TEXT("Handler found"), InvokeHandler(TEXT("gas.set_modifier_magnitude"), Payload));
    return true;
}

// ============================================================================
// GAS — gas.add_effect_execution_calculation  (REQ: blueprintPath, calculationClass)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGASAddEffectExecutionCalculationValidParamsNoCrashTest,
    "PinWright.gas.add_effect_execution_calculation.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGASAddEffectExecutionCalculationValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("blueprintPath"), TEXT("/Game/GAS/GE_Burn"));
    Payload->SetStringField(TEXT("calculationClass"), TEXT("/Game/GAS/EC_DamageCalc"));
    TestTrue(TEXT("Handler found"), InvokeHandler(TEXT("gas.add_effect_execution_calculation"), Payload));
    return true;
}

// ============================================================================
// GAS — gas.add_effect_cue  (REQ: blueprintPath, cueTag)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGASAddEffectCueValidParamsNoCrashTest,
    "PinWright.gas.add_effect_cue.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGASAddEffectCueValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("blueprintPath"), TEXT("/Game/GAS/GE_Burn"));
    Payload->SetStringField(TEXT("cueTag"), TEXT("GameplayCue.Fire.Burn"));
    TestTrue(TEXT("Handler found"), InvokeHandler(TEXT("gas.add_effect_cue"), Payload));
    return true;
}

// ============================================================================
// GAS — gas.set_effect_stacking  (REQ: blueprintPath)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGASSetEffectStackingValidParamsNoCrashTest,
    "PinWright.gas.set_effect_stacking.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGASSetEffectStackingValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("blueprintPath"), TEXT("/Game/GAS/GE_Burn"));
    Payload->SetStringField(TEXT("stackingType"), TEXT("aggregate_by_target"));
    Payload->SetNumberField(TEXT("stackLimit"), 5.0);
    TestTrue(TEXT("Handler found"), InvokeHandler(TEXT("gas.set_effect_stacking"), Payload));
    return true;
}

// ============================================================================
// GAS — gas.set_effect_tags  (REQ: blueprintPath)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGASSetEffectTagsValidParamsNoCrashTest,
    "PinWright.gas.set_effect_tags.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGASSetEffectTagsValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("blueprintPath"), TEXT("/Game/GAS/GE_Burn"));
    TestTrue(TEXT("Handler found"), InvokeHandler(TEXT("gas.set_effect_tags"), Payload));
    return true;
}

// ============================================================================
// GAS — gas.create_gameplay_cue_notify  (REQ: name)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGASCreateGameplayCueNotifyValidParamsNoCrashTest,
    "PinWright.gas.create_gameplay_cue_notify.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGASCreateGameplayCueNotifyValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    RunGASCreateHandlerAtUniquePath(*this, TEXT("gas.create_gameplay_cue_notify"), TEXT("GCN_FireImpact"));
    return true;
}

// ============================================================================
// GAS — gas.add_tag_to_asset  (REQ: assetPath, tag)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGASAddTagToAssetValidParamsNoCrashTest,
    "PinWright.gas.add_tag_to_asset.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGASAddTagToAssetValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), TEXT("/Game/GAS/GA_FireBolt"));
    Payload->SetStringField(TEXT("tag"), TEXT("Ability.Fire"));
    TestTrue(TEXT("Handler found"), InvokeHandler(TEXT("gas.add_tag_to_asset"), Payload));
    return true;
}

// An Actor blueprint whose SCS owns an AbilitySystemComponent must be REJECTED with
// UNSUPPORTED_TYPE. The old Actor-with-ASC branch fabricated a blank, unread
// "OwnedGameplayTags" member variable, structurally modified + saved the blueprint, and
// reported tagAdded:true while never writing the requested tag anywhere. The fixture is
// built from handlers only (blueprint.create + blueprint.scs.add_component) rather than a
// local CreateBlueprint helper, so it cannot ODR-collide with the equivalent fixture in
// Tests/Gameplay/TestGASHandlers.cpp when Unity merges the two TUs.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGASAddTagToAssetAscOwnerRejectedTest,
    "PinWright.gas.add_tag_to_asset.AscOwnerRejected",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGASAddTagToAssetAscOwnerRejectedTest::RunTest(const FString& Parameters)
{
    const FString Name = FString::Printf(TEXT("BP_AscTagOwner_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    const FString PackagePath = FString::Printf(TEXT("/Game/__PW_GatewayTests/%s"), *Name);

    TSharedPtr<FJsonObject> CreatePayload = MakeShared<FJsonObject>();
    CreatePayload->SetStringField(TEXT("name"), Name);
    CreatePayload->SetStringField(TEXT("savePath"), TEXT("/Game/__PW_GatewayTests"));
    CreatePayload->SetStringField(TEXT("parentClass"), TEXT("Character"));
    FTestResponseCapture CreateCapture;
    TestTrue(TEXT("blueprint.create handler found"),
        InvokeHandlerWithCapture(TEXT("blueprint.create"), CreatePayload, CreateCapture));
    if (!TestTrue(TEXT("blueprint.create succeeded"), CreateCapture.bSuccess))
    {
        return true;
    }
    ON_SCOPE_EXIT
    {
        CleanupTestAsset(PackagePath);
    };

    TSharedPtr<FJsonObject> AddPayload = MakeShared<FJsonObject>();
    AddPayload->SetStringField(TEXT("blueprintPath"), ToObjectPath(PackagePath));
    AddPayload->SetStringField(TEXT("componentClass"), TEXT("AbilitySystemComponent"));
    AddPayload->SetStringField(TEXT("componentName"), TEXT("AbilitySystem"));
    FTestResponseCapture AddCapture;
    TestTrue(TEXT("blueprint.scs.add_component handler found"),
        InvokeHandlerWithCapture(TEXT("blueprint.scs.add_component"), AddPayload, AddCapture));
    if (!AddCapture.bSuccess)
    {
        // No ASC class to attach means the GameplayAbilities plugin is unavailable, in
        // which case gas.add_tag_to_asset short-circuits with GAS_NOT_AVAILABLE and there
        // is nothing to assert here.
        PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-unavailable"),
            FString::Printf(
                TEXT("Could not attach an AbilitySystemComponent (%s); skipping ASC-owner rejection check."),
                *AddCapture.ErrorCode));
        return true;
    }

    // add_tag_to_asset resolves the tag before it dispatches on target type, so the tag has
    // to be registered or the call would fail with INVALID_TAG instead of UNSUPPORTED_TYPE.
    const FString TagName = TEXT("PinWrightTest.State.Buffed");
    if (IGameplayTagsEditorModule::IsAvailable())
    {
        IGameplayTagsEditorModule::Get().AddTransientEditorGameplayTag(TagName);
    }
    if (!TestTrue(TEXT("test tag is registered"),
            UGameplayTagsManager::Get().RequestGameplayTag(FName(*TagName), false).IsValid()))
    {
        return true;
    }

    TSharedPtr<FJsonObject> TagPayload = MakeShared<FJsonObject>();
    TagPayload->SetStringField(TEXT("assetPath"), ToObjectPath(PackagePath));
    TagPayload->SetStringField(TEXT("tag"), TagName);
    FTestResponseCapture TagCapture;
    TestTrue(TEXT("gas.add_tag_to_asset handler found"),
        InvokeHandlerWithCapture(TEXT("gas.add_tag_to_asset"), TagPayload, TagCapture));
    TestFalse(TEXT("ASC-owning actor is not reported as a successful tag write"),
        TagCapture.bSuccess);
    TestEqual(TEXT("ASC-owning actor rejected with UNSUPPORTED_TYPE"),
        TagCapture.ErrorCode, FString(TEXT("UNSUPPORTED_TYPE")));
    return true;
}

// ============================================================================
// GAS — gas.get_gas_info  (REQ: assetPath)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGASGetGasInfoValidParamsNoCrashTest,
    "PinWright.gas.get_gas_info.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGASGetGasInfoValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), TEXT("/Game/GAS/GA_FireBolt"));
    TestTrue(TEXT("Handler found"), InvokeHandler(TEXT("gas.get_gas_info"), Payload));
    return true;
}

// ============================================================================
// GAS — gas.create_ability_set  (no required params)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGASCreateAbilitySetValidParamsNoCrashTest,
    "PinWright.gas.create_ability_set.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGASCreateAbilitySetValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("setName"), TEXT("TestAbilitySet"));
    TestTrue(TEXT("Handler found"), InvokeHandler(TEXT("gas.create_ability_set"), Payload));
    return true;
}

// ============================================================================
// GAS — gas.create_execution_calculation  (REQ: name)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGASCreateExecutionCalculationValidParamsNoCrashTest,
    "PinWright.gas.create_execution_calculation.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGASCreateExecutionCalculationValidParamsNoCrashTest::RunTest(const FString& Parameters)
{
    RunGASCreateHandlerAtUniquePath(*this, TEXT("gas.create_execution_calculation"), TEXT("EC_DamageCalc"));
    return true;
}