// Copyright (c) 2026 Alexander Penkin. MIT License.

// Unit tests for GAS handlers:
//   gas.set_effect_period (round-trip + Instant-rejection counterfactual)
//   gas.add_attribute (compiles the generated class)
//   gas.set_effect_tags (rejects unregistered tags — E-gas-set-effect-tags-drops-unregistered)
//   gas.set_ability_cooldown / gas.set_ability_costs (a plain blueprint-asset path
//     persists the GameplayEffect class to the CDO and a non-resolving path is
//     rejected NOT_FOUND, not silent-success —
//     B-set-ability-cooldown-cost-class-path-silent-drop)
//   gas.set_effect_tags (grants tags through the 5.3+ component model so
//     GetGrantedTags() is non-empty — E-gas-set-effect-tags-writes-deprecated-container-not-component)
//   gas.add_effect_execution_calculation (accepts the bare exec-calc asset path
//     without a _C suffix and rejects a non-exec-calc class with INVALID_TYPE —
//     E-gas-add-execution-calc-requires-c-suffix)
//   gas.set_ability_input (an ABSENT target property reports PROPERTY_NOT_FOUND,
//     distinct from the present-but-wrong-type PROPERTY_NOT_INT —
//     E-set-ability-input-not-found-mislabeled)
//
// Counterfactual coverage: if the Period assignment line is removed from
// gas.set_effect_period, the CDO retains its default Period.Value == 0, so the
// round-trip test's "Period.Value ≈ 0.5" assertion fails. If the NOT_PERIODIC
// check is incorrectly applied to has_duration effects, the round-trip test
// errors out before reaching the field assertions.
//
// Regression coverage for E-gas-info-skips-asc-owner-actor (get_gas_info
// readback gap):
//   - get_gas_info.AbilitySystemOwnerEmitsGasType: an actor blueprint that owns
//     an ASC must read back gasType:"AbilitySystemOwner" + an
//     abilitySystemComponents list (name + replicationMode). Reverting the
//     ASC-owner branch drops gasType and the list, failing this test.
//   - get_gas_info.GameplayEffectEmitsExecutionsAndTags: the GameplayEffect
//     branch must echo executionCount/executionClasses (from EffectCDO->Executions)
//     and grantedTags (from InheritableOwnedTagsContainer). Reverting the GE-branch
//     additions drops those fields, failing this test.
//
// Regression coverage for E-gas-info-ability-omits-cooldown-cost-tags:
//   - get_gas_info.AbilityEmitsCooldownCostTags: the GameplayAbility branch must
//     echo cooldownEffect / costEffect (the CDO's CooldownGameplayEffectClass /
//     CostGameplayEffectClass object paths, written by set_ability_cooldown /
//     set_ability_costs) and an abilityTags array (set_ability_tags). Reverting the
//     ability-branch additions drops those fields, failing this test.
//
// Regression coverage for F-gas-ability-activation-tags-unauthorable:
//   - set_ability_tags.ActivationTagsAuthoredAndReadBack: gas.set_ability_tags must
//     author the three owner-state activation containers via its new
//     activationBlockedTags / activationRequiredTags / activationOwnedTags params, and
//     get_gas_info must read them back. Reverting either the write loops or the
//     get_gas_info emits leaves the readback arrays empty/absent, failing this test.
//
// Regression coverage for E-gas-info-effect-modifiers-duration-value-thin:
//   - get_gas_info.EffectModifiersDurationAndAttributeSetAttributes: the
//     GameplayEffect branch must echo durationMagnitude (the seconds set_effect_duration
//     wrote), a readable durationPolicyName, and a modifiers array (per-modifier
//     operation/magnitude/attribute from add_effect_modifier/set_modifier_magnitude),
//     and the AttributeSet branch must echo an attributes array (name + baseValue from
//     add_attribute/set_attribute_base_value). Reverting any of those emits fails the
//     matching assertion.

#include "Misc/AutomationTest.h"

#include "Dom/JsonObject.h"
#include "EditorAssetLibrary.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Character.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "Tests/TestUtils.h"
#include "Tests/TestSkipReporting.h"
#include "UObject/Package.h"

#if __has_include("AbilitySystemComponent.h")
#define MCP_GAS_TEST_HAS_GAS 1
#include "AbilitySystemComponent.h"
#include "GameplayEffect.h"
#include "GameplayEffectExecutionCalculation.h"  // complete type for Executions[].CalculationClass.Get()
#include "AttributeSet.h"
#include "GameplayTagsManager.h"
#include "GameplayTagsEditorModule.h"  // AddTransientEditorGameplayTag — register the owned tag for this session
#else
#define MCP_GAS_TEST_HAS_GAS 0
#endif

namespace
{
    FString MakeUniqueGEPath(const TCHAR* Prefix)
    {
        return FString::Printf(
            TEXT("/Game/__PW_GatewayTests/%s_%s"),
            Prefix,
            *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    }

    // ToObjectPath now lives in Tests/TestUtils.h (included above) so the
    // "%s.%s" object-path contract has a single definition; the former local
    // anonymous-namespace copy was byte-identical and collided (ambiguous call)
    // with the shared one once it was centralized.

    // Create a GAS asset (GameplayEffect / AttributeSet / GameplayAbility) via the
    // matching gas.create_* handler. All three handlers share the same {name, path}
    // contract — the only variation is the method string and CreateEffect's extra
    // durationType field (passed via the optional AddExtra hook). Returns the
    // package path on success, empty on failure. The shared assertion labels are
    // derived from Method so the failure diagnostics stay specific.
    FString CreateGASAsset(
        FAutomationTestBase& Test,
        const TCHAR* Method,
        const TCHAR* NamePrefix,
        TFunctionRef<void(FJsonObject&)> AddExtra)
    {
        const FString PackagePath = MakeUniqueGEPath(NamePrefix);
        const FString Name = FPackageName::GetLongPackageAssetName(PackagePath);
        const FString Path = FPackageName::GetLongPackagePath(PackagePath);

        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("name"), Name);
        Payload->SetStringField(TEXT("path"), Path);
        AddExtra(*Payload);

        FTestResponseCapture Capture;
        if (!Test.TestTrue(FString::Printf(TEXT("%s handler found"), Method),
                InvokeHandlerWithCapture(Method, Payload, Capture)))
        {
            return FString();
        }
        if (!Test.TestTrue(FString::Printf(TEXT("%s succeeded"), Method), Capture.bSuccess))
        {
            Test.AddError(FString::Printf(TEXT("%s failed: %s %s"),
                Method, *Capture.ErrorCode, *Capture.Message));
            return FString();
        }
        return PackagePath;
    }

    // Create a GameplayEffect blueprint via gas.create_gameplay_effect.
    // Returns the package path on success, empty on failure.
    FString CreateEffect(
        FAutomationTestBase& Test,
        const TCHAR* NamePrefix,
        const FString& DurationType)
    {
        return CreateGASAsset(Test, TEXT("gas.create_gameplay_effect"), NamePrefix,
            [&DurationType](FJsonObject& Payload)
            {
                Payload.SetStringField(TEXT("durationType"), DurationType);
            });
    }

    // Create an AttributeSet blueprint via gas.create_attribute_set.
    // Returns the package path on success, empty on failure.
    FString CreateAttributeSet(FAutomationTestBase& Test, const TCHAR* NamePrefix)
    {
        return CreateGASAsset(Test, TEXT("gas.create_attribute_set"), NamePrefix,
            [](FJsonObject&) {});
    }

    // Create a GameplayAbility blueprint via gas.create_gameplay_ability.
    // Returns the package path on success, empty on failure.
    FString CreateAbility(FAutomationTestBase& Test, const TCHAR* NamePrefix)
    {
        return CreateGASAsset(Test, TEXT("gas.create_gameplay_ability"), NamePrefix,
            [](FJsonObject&) {});
    }

#if MCP_GAS_TEST_HAS_GAS
    // Build a real (loadable-by-path) Character blueprint that owns an ASC named
    // ComponentName, via CreatePackage + FKismetEditorUtilities::CreateBlueprint
    // (ACharacter is the canonical ASC-owner parent) + the blueprint.scs.add_component
    // write handler. The caller owns PackagePath (so it can wire its own cleanup) and
    // is responsible for any compile step. Returns the Blueprint on success, nullptr on
    // failure (assertions/errors already added).
    UBlueprint* CreateAscOwnerBlueprint(
        FAutomationTestBase& Test,
        const FString& PackagePath,
        const TCHAR* ComponentName)
    {
        const FString AssetName = FPackageName::GetLongPackageAssetName(PackagePath);

        UPackage* Package = CreatePackage(*PackagePath);
        if (!Test.TestNotNull(TEXT("package allocated"), Package))
        {
            return nullptr;
        }

        UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(
            ACharacter::StaticClass(),
            Package,
            FName(*AssetName),
            BPTYPE_Normal,
            UBlueprint::StaticClass(),
            UBlueprintGeneratedClass::StaticClass());
        if (!Test.TestNotNull(TEXT("character blueprint created"), Blueprint))
        {
            return nullptr;
        }

        TSharedPtr<FJsonObject> AddPayload = MakeShared<FJsonObject>();
        AddPayload->SetStringField(TEXT("blueprintPath"), ToObjectPath(PackagePath));
        AddPayload->SetStringField(TEXT("componentClass"), TEXT("AbilitySystemComponent"));
        AddPayload->SetStringField(TEXT("componentName"), ComponentName);
        FTestResponseCapture AddCapture;
        Test.TestTrue(TEXT("blueprint.scs.add_component handler found"),
            InvokeHandlerWithCapture(TEXT("blueprint.scs.add_component"), AddPayload, AddCapture));
        if (!Test.TestTrue(TEXT("blueprint.scs.add_component succeeded"), AddCapture.bSuccess))
        {
            Test.AddError(FString::Printf(TEXT("scs.add_component failed: %s %s"),
                *AddCapture.ErrorCode, *AddCapture.Message));
            return nullptr;
        }
        return Blueprint;
    }
#endif

    // Returns the first abilitySystemComponents entry, or null. The handler emits
    // exactly one entry per ASC; reading the first is robust to any SCS name
    // normalization on compile while still proving the list is populated.
    TSharedPtr<FJsonObject> FirstAscEntry(const TSharedPtr<FJsonObject>& Result)
    {
        if (!Result.IsValid())
        {
            return nullptr;
        }
        const TArray<TSharedPtr<FJsonValue>>* Components = nullptr;
        if (!Result->TryGetArrayField(TEXT("abilitySystemComponents"), Components)
            || !Components || Components->Num() == 0)
        {
            return nullptr;
        }
        return (*Components)[0].IsValid() ? (*Components)[0]->AsObject() : nullptr;
    }
}

// ============================================================================
// gas.set_effect_period — round-trip
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGasSetEffectPeriodRoundTripTest,
    "PinWright.gas.set_effect_period.RoundTrip",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGasSetEffectPeriodRoundTripTest::RunTest(const FString& Parameters)
{
#if !MCP_GAS_TEST_HAS_GAS
    PinWrightTestSkip::SkipAssertions(*this, TEXT("optional-plugin-not-shipped"),
        TEXT("GameplayAbilities plugin not available; skipping."));
    return true;
#else
    const FString PackagePath = CreateEffect(*this, TEXT("GE_PeriodicHeal"), TEXT("has_duration"));
    if (PackagePath.IsEmpty())
    {
        return true;
    }
    ON_SCOPE_EXIT
    {
        CleanupTestAsset(PackagePath);
    };

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("blueprintPath"), ToObjectPath(PackagePath));
    Payload->SetNumberField(TEXT("periodSeconds"), 0.5);
    Payload->SetBoolField(TEXT("executeOnApplication"), true);
    Payload->SetStringField(TEXT("inhibitionPolicy"), TEXT("reset_period"));

    FTestResponseCapture Capture;
    TestTrue(TEXT("gas.set_effect_period handler found"),
        InvokeHandlerWithCapture(TEXT("gas.set_effect_period"), Payload, Capture));
    if (!TestTrue(TEXT("gas.set_effect_period succeeded"), Capture.bSuccess))
    {
        AddError(FString::Printf(TEXT("set_effect_period failed: %s %s"),
            *Capture.ErrorCode, *Capture.Message));
        return true;
    }

    UBlueprint* Blueprint = Cast<UBlueprint>(UEditorAssetLibrary::LoadAsset(ToObjectPath(PackagePath)));
    if (!TestNotNull(TEXT("effect blueprint loads"), Blueprint))
    {
        return true;
    }
    if (!TestNotNull(TEXT("effect generated class exists"),
            Blueprint ? Blueprint->GeneratedClass.Get() : nullptr))
    {
        return true;
    }

    UGameplayEffect* EffectCDO = Cast<UGameplayEffect>(Blueprint->GeneratedClass->GetDefaultObject());
    if (!TestNotNull(TEXT("effect CDO casts to UGameplayEffect"), EffectCDO))
    {
        return true;
    }

    TestTrue(TEXT("Period.Value matches periodSeconds"),
        FMath::IsNearlyEqual(EffectCDO->Period.Value, 0.5f));
    TestTrue(TEXT("bExecutePeriodicEffectOnApplication is true"),
        EffectCDO->bExecutePeriodicEffectOnApplication);
    TestEqual(TEXT("PeriodicInhibitionPolicy is ResetPeriod"),
        static_cast<uint8>(EffectCDO->PeriodicInhibitionPolicy),
        static_cast<uint8>(EGameplayEffectPeriodInhibitionRemovedPolicy::ResetPeriod));

    return true;
#endif
}

// ============================================================================
// gas.set_effect_period — Instant effects are rejected
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGasSetEffectPeriodInstantRejectedTest,
    "PinWright.gas.set_effect_period.InstantRejected",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGasSetEffectPeriodInstantRejectedTest::RunTest(const FString& Parameters)
{
#if !MCP_GAS_TEST_HAS_GAS
    PinWrightTestSkip::SkipAssertions(*this, TEXT("optional-plugin-not-shipped"),
        TEXT("GameplayAbilities plugin not available; skipping."));
    return true;
#else
    const FString PackagePath = CreateEffect(*this, TEXT("GE_InstantNoPeriod"), TEXT("instant"));
    if (PackagePath.IsEmpty())
    {
        return true;
    }
    ON_SCOPE_EXIT
    {
        CleanupTestAsset(PackagePath);
    };

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("blueprintPath"), ToObjectPath(PackagePath));
    Payload->SetNumberField(TEXT("periodSeconds"), 1.0);

    FTestResponseCapture Capture;
    TestTrue(TEXT("gas.set_effect_period handler found"),
        InvokeHandlerWithCapture(TEXT("gas.set_effect_period"), Payload, Capture));
    TestFalse(TEXT("set_effect_period rejects Instant effects"), Capture.bSuccess);
    TestEqual(TEXT("error code is NOT_PERIODIC"),
        Capture.ErrorCode, FString(TEXT("NOT_PERIODIC")));

    return true;
#endif
}

// ============================================================================
// gas.add_attribute compiles the AttributeSet so the attribute lands on the
// generated class (E-gas-execution-capture-attribute-set-prereq-undocumented).
//
// Counterfactual: if the FKismetEditorUtilities::CompileBlueprint call is removed
// from gas.add_attribute, the AttributeSet generated class has no FProperty for
// the added attribute, so FindPropertyByName below returns null and this test
// fails — which is the exact ATTRIBUTE_NOT_FOUND trap the ticket documents. The
// fix has gas.add_attribute compile the AttributeSet so the added property is
// visible to downstream readers such as gas.set_attribute_base_value.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGasAddAttributeCompilesGeneratedClassTest,
    "PinWright.gas.add_attribute.CompilesGeneratedClass",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGasAddAttributeCompilesGeneratedClassTest::RunTest(const FString& Parameters)
{
#if !MCP_GAS_TEST_HAS_GAS
    PinWrightTestSkip::SkipAssertions(*this, TEXT("optional-plugin-not-shipped"),
        TEXT("GameplayAbilities plugin not available; skipping."));
    return true;
#else
    const FString PackagePath = CreateAttributeSet(*this, TEXT("AS_AddAttr"));
    if (PackagePath.IsEmpty())
    {
        return true;
    }
    ON_SCOPE_EXIT
    {
        CleanupTestAsset(PackagePath);
    };

    // Add an attribute. The fix compiles the blueprint here; the test below
    // deliberately does NOT issue a separate blueprint.compile.
    const FString ObjectPath = ToObjectPath(PackagePath);
    TSharedPtr<FJsonObject> AddPayload = MakeShared<FJsonObject>();
    AddPayload->SetStringField(TEXT("blueprintPath"), ObjectPath);
    AddPayload->SetStringField(TEXT("attributeName"), TEXT("AttackPower"));

    FTestResponseCapture AddCapture;
    TestTrue(TEXT("gas.add_attribute handler found"),
        InvokeHandlerWithCapture(TEXT("gas.add_attribute"), AddPayload, AddCapture));
    if (!TestTrue(TEXT("gas.add_attribute succeeded"), AddCapture.bSuccess))
    {
        AddError(FString::Printf(TEXT("add_attribute failed: %s %s"),
            *AddCapture.ErrorCode, *AddCapture.Message));
        return true;
    }

    // The load-bearing assertion: the attribute must be a real FProperty on the
    // COMPILED generated class, with no manual compile between add and read.
    UBlueprint* Blueprint = Cast<UBlueprint>(UEditorAssetLibrary::LoadAsset(ObjectPath));
    if (!TestNotNull(TEXT("attribute set blueprint loads"), Blueprint))
    {
        return true;
    }
    UClass* GeneratedClass = Blueprint->GeneratedClass.Get();
    if (!TestNotNull(TEXT("attribute set generated class exists"), GeneratedClass))
    {
        return true;
    }

    FProperty* AttrProperty = GeneratedClass->FindPropertyByName(FName(TEXT("AttackPower")));
    TestNotNull(
        TEXT("AttackPower is a compiled FProperty on the generated class (gas.add_attribute compiled it)"),
        AttrProperty);

    return true;
#endif
}

// ============================================================================
// gas.add_effect_modifier / gas.set_modifier_attribute — bind a modifier to the
// attribute it modifies (F-gas-modifier-no-attribute-binding).
//
// Before the fix, gas.add_effect_modifier exposed no attribute param and never set
// FGameplayModifierInfo.Attribute, so every modifier landed with an empty (invalid)
// FGameplayAttribute and was inert. This builds a real AttributeSet (AttackPower),
// a GameplayEffect, then adds a modifier bound to AttackPower and asserts the live
// CDO modifier carries a VALID attribute named AttackPower — and that
// gas.set_modifier_attribute can rebind an existing modifier.
//
// Counterfactual: revert the Modifier.Attribute assignment (or drop the attribute
// param) and the modifier's Attribute stays default-constructed → IsValid() is
// false and GetName() is empty, failing the assertions below. A wrong attribute
// spec must still be REJECTED (ATTRIBUTE_NOT_FOUND), not silently accepted.
//
// Second counterfactual, for gas.set_modifier_attribute specifically: delete its
// `Modifiers[ModifierIndex].Attribute = Attr` write and the handler still resolves
// everything, still succeeds, and still echoes the resolved name (derived from the
// INPUT spec, never read back from the CDO). Step 6 therefore rebinds to a SECOND
// attribute (DefensePower) and asserts the CDO now reads DefensePower — rebinding to
// the attribute step 3 already bound would assert a state that was already true.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGasAddEffectModifierBindsAttributeTest,
    "PinWright.gas.add_effect_modifier.BindsAttribute",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGasAddEffectModifierBindsAttributeTest::RunTest(const FString& Parameters)
{
#if !MCP_GAS_TEST_HAS_GAS
    PinWrightTestSkip::SkipAssertions(*this, TEXT("optional-plugin-not-shipped"),
        TEXT("GameplayAbilities plugin not available; skipping."));
    return true;
#else
    // 1. Build an AttributeSet with TWO attributes. gas.add_attribute compiles the
    //    generated class, so each lands as a real FProperty on <name>_C. The second
    //    attribute (DefensePower) exists so step 6 can rebind the modifier to a
    //    DIFFERENT attribute than step 3 bound — rebinding to the same one asserts a
    //    state that was already true and cannot catch a dropped CDO write.
    const FString AttrSetPath = CreateAttributeSet(*this, TEXT("AS_ModBind"));
    if (AttrSetPath.IsEmpty())
    {
        return true;
    }
    ON_SCOPE_EXIT { CleanupTestAsset(AttrSetPath); };

    const FString AttrSetObjectPath = ToObjectPath(AttrSetPath);
    const auto AddAttribute = [&](const TCHAR* AttributeName) -> bool
    {
        TSharedPtr<FJsonObject> AddPayload = MakeShared<FJsonObject>();
        AddPayload->SetStringField(TEXT("blueprintPath"), AttrSetObjectPath);
        AddPayload->SetStringField(TEXT("attributeName"), AttributeName);
        FTestResponseCapture AddCapture;
        TestTrue(TEXT("gas.add_attribute handler found"),
            InvokeHandlerWithCapture(TEXT("gas.add_attribute"), AddPayload, AddCapture));
        if (!TestTrue(FString::Printf(TEXT("gas.add_attribute %s succeeded"), AttributeName),
                AddCapture.bSuccess))
        {
            AddError(FString::Printf(TEXT("add_attribute %s failed: %s %s"),
                AttributeName, *AddCapture.ErrorCode, *AddCapture.Message));
            return false;
        }
        return true;
    };
    if (!AddAttribute(TEXT("AttackPower")) || !AddAttribute(TEXT("DefensePower")))
    {
        return true;
    }

    // The attribute spec the handlers resolve: '<AttributeSet generated class>.AttrName'.
    // The generated class object path is '<PackagePath>.<AssetName>_C'.
    const FString AttrSetName = FPackageName::GetLongPackageAssetName(AttrSetPath);
    const FString AttrSpec = FString::Printf(TEXT("%s.%s_C.AttackPower"), *AttrSetPath, *AttrSetName);
    const FString DefenseSpec = FString::Printf(TEXT("%s.%s_C.DefensePower"), *AttrSetPath, *AttrSetName);

    // 2. Build a GameplayEffect to carry the modifier.
    const FString EffectPath = CreateEffect(*this, TEXT("GE_ModBind"), TEXT("has_duration"));
    if (EffectPath.IsEmpty())
    {
        return true;
    }
    ON_SCOPE_EXIT { CleanupTestAsset(EffectPath); };
    const FString EffectObjectPath = ToObjectPath(EffectPath);

    // 3. Add a modifier bound to AttackPower.
    {
        TSharedPtr<FJsonObject> ModPayload = MakeShared<FJsonObject>();
        ModPayload->SetStringField(TEXT("blueprintPath"), EffectObjectPath);
        ModPayload->SetStringField(TEXT("operation"), TEXT("additive"));
        ModPayload->SetNumberField(TEXT("magnitude"), 25.0);
        ModPayload->SetStringField(TEXT("attribute"), AttrSpec);
        FTestResponseCapture ModCapture;
        TestTrue(TEXT("gas.add_effect_modifier handler found"),
            InvokeHandlerWithCapture(TEXT("gas.add_effect_modifier"), ModPayload, ModCapture));
        if (!TestTrue(TEXT("gas.add_effect_modifier succeeded"), ModCapture.bSuccess))
        {
            AddError(FString::Printf(TEXT("add_effect_modifier failed: %s %s"),
                *ModCapture.ErrorCode, *ModCapture.Message));
            return true;
        }
        // The result echoes the resolved attribute name back.
        FString EchoedAttr;
        ModCapture.Result->TryGetStringField(TEXT("attribute"), EchoedAttr);
        TestEqual(TEXT("add_effect_modifier echoes the resolved attribute"),
            EchoedAttr, FString(TEXT("AttackPower")));
    }

    // 4. The load-bearing assertion: the live modifier on the CDO is BOUND to a
    //    valid AttackPower attribute (not the default-constructed empty one).
    UBlueprint* Blueprint = Cast<UBlueprint>(UEditorAssetLibrary::LoadAsset(EffectObjectPath));
    if (!TestNotNull(TEXT("effect blueprint loads"), Blueprint)
        || !TestNotNull(TEXT("effect generated class exists"),
                Blueprint ? Blueprint->GeneratedClass.Get() : nullptr))
    {
        return true;
    }
    UGameplayEffect* EffectCDO = Cast<UGameplayEffect>(Blueprint->GeneratedClass->GetDefaultObject());
    if (!TestNotNull(TEXT("effect CDO casts to UGameplayEffect"), EffectCDO))
    {
        return true;
    }
    if (!TestEqual(TEXT("effect has exactly one modifier"), EffectCDO->Modifiers.Num(), 1))
    {
        return true;
    }
    TestTrue(TEXT("modifier Attribute is valid (bound, not the empty default)"),
        EffectCDO->Modifiers[0].Attribute.IsValid());
    TestEqual(TEXT("modifier Attribute is AttackPower"),
        EffectCDO->Modifiers[0].Attribute.GetName(), FString(TEXT("AttackPower")));

    // 5. A bad attribute spec must be REJECTED, not silently accepted as empty.
    {
        TSharedPtr<FJsonObject> BadPayload = MakeShared<FJsonObject>();
        BadPayload->SetStringField(TEXT("blueprintPath"), EffectObjectPath);
        BadPayload->SetStringField(TEXT("attribute"),
            FString::Printf(TEXT("%s.%s_C.NoSuchAttribute"), *AttrSetPath, *AttrSetName));
        FTestResponseCapture BadCapture;
        TestTrue(TEXT("gas.add_effect_modifier handler found (bad attr)"),
            InvokeHandlerWithCapture(TEXT("gas.add_effect_modifier"), BadPayload, BadCapture));
        TestFalse(TEXT("add_effect_modifier rejects an unknown attribute"), BadCapture.bSuccess);
        TestEqual(TEXT("error code is ATTRIBUTE_NOT_FOUND"),
            BadCapture.ErrorCode, FString(TEXT("ATTRIBUTE_NOT_FOUND")));
    }

    // 6. gas.set_modifier_attribute rebinds the existing modifier to a DIFFERENT
    //    attribute (DefensePower, not the AttackPower step 3 bound), then reads the
    //    binding back off the CDO. Reading the response echo proves nothing: the
    //    handler derives it from the input spec, so it still reports the new name
    //    even if the `Modifiers[i].Attribute = Attr` write is dropped. Only the CDO
    //    readback below distinguishes a real rebind from a no-op.
    {
        TSharedPtr<FJsonObject> SetPayload = MakeShared<FJsonObject>();
        SetPayload->SetStringField(TEXT("blueprintPath"), EffectObjectPath);
        SetPayload->SetNumberField(TEXT("modifierIndex"), 0);
        SetPayload->SetStringField(TEXT("attribute"), DefenseSpec);
        FTestResponseCapture SetCapture;
        TestTrue(TEXT("gas.set_modifier_attribute handler found"),
            InvokeHandlerWithCapture(TEXT("gas.set_modifier_attribute"), SetPayload, SetCapture));
        if (TestTrue(TEXT("gas.set_modifier_attribute succeeded"), SetCapture.bSuccess))
        {
            FString SetEcho;
            SetCapture.Result->TryGetStringField(TEXT("attribute"), SetEcho);
            TestEqual(TEXT("set_modifier_attribute echoes DefensePower"),
                SetEcho, FString(TEXT("DefensePower")));
        }
        TestTrue(TEXT("modifier still bound to a valid attribute after set_modifier_attribute"),
            EffectCDO->Modifiers[0].Attribute.IsValid());

        // The load-bearing rebind assertion: re-read the CDO (not the echo) and
        // require the modifier to carry the NEW attribute.
        UGameplayEffect* ReboundCDO =
            Cast<UGameplayEffect>(Blueprint->GeneratedClass->GetDefaultObject());
        if (TestNotNull(TEXT("effect CDO still casts after set_modifier_attribute"), ReboundCDO)
            && TestEqual(TEXT("effect still has exactly one modifier after rebind"),
                    ReboundCDO->Modifiers.Num(), 1))
        {
            TestTrue(TEXT("rebound modifier Attribute is valid"),
                ReboundCDO->Modifiers[0].Attribute.IsValid());
            TestEqual(
                TEXT("CDO modifier Attribute is DefensePower after rebind (read from the CDO, not the echo)"),
                ReboundCDO->Modifiers[0].Attribute.GetName(), FString(TEXT("DefensePower")));
        }
    }

    return true;
#endif
}

// ============================================================================
// gas.set_effect_tags rejects unregistered tags instead of silently dropping them
// (E-gas-set-effect-tags-drops-unregistered).
//
// Counterfactual: if the validate-before-mutate guard is reverted to the old
// `GetOrRequestTag` + `if (Tag.IsValid())` silent-skip, an unregistered tag
// produces ok:true with tagsAdded:[] and no error — so Capture.bSuccess is true
// and ErrorCode is empty, failing every assertion below. The fix makes the call
// reject with INVALID_PARAMS and report the offending tag under droppedTags.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGasSetEffectTagsRejectsUnregisteredTest,
    "PinWright.gas.set_effect_tags.RejectsUnregistered",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGasSetEffectTagsRejectsUnregisteredTest::RunTest(const FString& Parameters)
{
#if !MCP_GAS_TEST_HAS_GAS
    PinWrightTestSkip::SkipAssertions(*this, TEXT("optional-plugin-not-shipped"),
        TEXT("GameplayAbilities plugin not available; skipping."));
    return true;
#else
    const FString PackagePath = CreateEffect(*this, TEXT("GE_TagDrop"), TEXT("has_duration"));
    if (PackagePath.IsEmpty())
    {
        return true;
    }
    ON_SCOPE_EXIT
    {
        CleanupTestAsset(PackagePath);
    };

    // A GUID-suffixed tag that is guaranteed absent from the project tag registry.
    const FString UnregisteredTag = FString::Printf(
        TEXT("Test.Unregistered.%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("blueprintPath"), ToObjectPath(PackagePath));
    TArray<TSharedPtr<FJsonValue>> GrantedTags;
    GrantedTags.Add(MakeShared<FJsonValueString>(UnregisteredTag));
    Payload->SetArrayField(TEXT("grantedTags"), GrantedTags);

    FTestResponseCapture Capture;
    TestTrue(TEXT("gas.set_effect_tags handler found"),
        InvokeHandlerWithCapture(TEXT("gas.set_effect_tags"), Payload, Capture));

    // The load-bearing assertions: the call must NOT report unqualified success,
    // it must reject with INVALID_PARAMS, and it must name the dropped tag.
    TestFalse(TEXT("set_effect_tags rejects an unregistered tag (no silent success)"),
        Capture.bSuccess);
    TestEqual(TEXT("error code is INVALID_PARAMS"),
        Capture.ErrorCode, FString(TEXT("INVALID_PARAMS")));
    TestTrue(TEXT("droppedTags array names the unregistered tag"),
        JsonStringArrayContains(Capture.Result, TEXT("droppedTags"), UnregisteredTag));

    return true;
#endif
}

// ============================================================================
// gas.get_gas_info — ASC-owning actor blueprint reads back as AbilitySystemOwner
// Regression for E-gas-info-skips-asc-owner-actor (#1).
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGasGetGasInfoAbilitySystemOwnerTest,
    "PinWright.gas.get_gas_info.AbilitySystemOwnerEmitsGasType",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGasGetGasInfoAbilitySystemOwnerTest::RunTest(const FString& Parameters)
{
#if !MCP_GAS_TEST_HAS_GAS
    PinWrightTestSkip::SkipAssertions(*this, TEXT("optional-plugin-not-shipped"),
        TEXT("GameplayAbilities plugin not available; skipping."));
    return true;
#else
    // Build a real (loadable-by-path) Character blueprint so get_gas_info's
    // LoadObject resolves it. Character is the canonical ASC-owner parent.
    const FString PackagePath = MakeUniqueGEPath(TEXT("BP_GASHero"));
    ON_SCOPE_EXIT { CleanupTestAsset(PackagePath); };

    // Add the ASC through the production write handler (the same call a caller
    // makes); compile below so the generated-class CDO is current.
    UBlueprint* Blueprint = CreateAscOwnerBlueprint(*this, PackagePath, TEXT("AbilitySystem"));
    if (!Blueprint)
    {
        return true;
    }
    FKismetEditorUtilities::CompileBlueprint(Blueprint);

    // The defect: before the ASC-owner branch, this returns bare metadata with
    // no gasType. After the fix it must carry gasType + the ASC component.
    TSharedPtr<FJsonObject> InfoPayload = MakeShared<FJsonObject>();
    InfoPayload->SetStringField(TEXT("assetPath"), ToObjectPath(PackagePath));
    FTestResponseCapture InfoCapture;
    TestTrue(TEXT("gas.get_gas_info handler found"),
        InvokeHandlerWithCapture(TEXT("gas.get_gas_info"), InfoPayload, InfoCapture));
    if (!TestTrue(TEXT("gas.get_gas_info succeeded"), InfoCapture.bSuccess))
    {
        AddError(FString::Printf(TEXT("get_gas_info failed: %s %s"),
            *InfoCapture.ErrorCode, *InfoCapture.Message));
        return true;
    }

    FString GasType;
    InfoCapture.Result->TryGetStringField(TEXT("gasType"), GasType);
    TestEqual(TEXT("gasType is AbilitySystemOwner"), GasType, FString(TEXT("AbilitySystemOwner")));

    TSharedPtr<FJsonObject> AscEntry = FirstAscEntry(InfoCapture.Result);
    if (TestTrue(TEXT("abilitySystemComponents lists the ASC"), AscEntry.IsValid()))
    {
        FString AscName;
        AscEntry->TryGetStringField(TEXT("name"), AscName);
        TestEqual(TEXT("ASC entry carries the component name"), AscName, FString(TEXT("AbilitySystem")));
    }

    return true;
#endif
}

// ============================================================================
// gas.get_gas_info — GameplayEffect branch echoes executions + granted tags
// Regression for E-gas-info-skips-asc-owner-actor (#2).
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGasGetGasInfoEffectExecutionsAndTagsTest,
    "PinWright.gas.get_gas_info.GameplayEffectEmitsExecutionsAndTags",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGasGetGasInfoEffectExecutionsAndTagsTest::RunTest(const FString& Parameters)
{
#if !MCP_GAS_TEST_HAS_GAS
    PinWrightTestSkip::SkipAssertions(*this, TEXT("optional-plugin-not-shipped"),
        TEXT("GameplayAbilities plugin not available; skipping."));
    return true;
#else
    const FString PackagePath = CreateEffect(*this, TEXT("GE_FireballDamage"), TEXT("instant"));
    if (PackagePath.IsEmpty())
    {
        return true;
    }
    ON_SCOPE_EXIT { CleanupTestAsset(PackagePath); };

    // Wire an execution calculation (the engine's native base class is a valid,
    // loadable-by-path UGameplayEffectExecutionCalculation) onto the effect.
    {
        TSharedPtr<FJsonObject> ExecPayload = MakeShared<FJsonObject>();
        ExecPayload->SetStringField(TEXT("blueprintPath"), ToObjectPath(PackagePath));
        ExecPayload->SetStringField(TEXT("calculationClass"),
            TEXT("/Script/GameplayAbilities.GameplayEffectExecutionCalculation"));
        FTestResponseCapture ExecCapture;
        TestTrue(TEXT("gas.add_effect_execution_calculation handler found"),
            InvokeHandlerWithCapture(TEXT("gas.add_effect_execution_calculation"), ExecPayload, ExecCapture));
        TestTrue(TEXT("gas.add_effect_execution_calculation succeeded"), ExecCapture.bSuccess);
    }
    // Register the owned tag transiently for this editor session — set_effect_tags
    // silently drops unregistered tags, so depend on a self-registered tag rather
    // than a project-defined one (keeps the test green on a clean CI host).
    const FString OwnedTagName = TEXT("PinWrightTest.Effect.Damage.Fire");
    if (IGameplayTagsEditorModule::IsAvailable())
    {
        IGameplayTagsEditorModule::Get().AddTransientEditorGameplayTag(OwnedTagName);
    }
    if (!TestTrue(TEXT("owned tag is registered"),
            UGameplayTagsManager::Get().RequestGameplayTag(FName(*OwnedTagName), false).IsValid()))
    {
        return true;
    }
    {
        TSharedPtr<FJsonObject> TagPayload = MakeShared<FJsonObject>();
        TagPayload->SetStringField(TEXT("blueprintPath"), ToObjectPath(PackagePath));
        TArray<TSharedPtr<FJsonValue>> TagsArr;
        TagsArr.Add(MakeShared<FJsonValueString>(OwnedTagName));
        TagPayload->SetArrayField(TEXT("grantedTags"), TagsArr);
        FTestResponseCapture TagCapture;
        TestTrue(TEXT("gas.set_effect_tags handler found"),
            InvokeHandlerWithCapture(TEXT("gas.set_effect_tags"), TagPayload, TagCapture));
        TestTrue(TEXT("gas.set_effect_tags succeeded"), TagCapture.bSuccess);
    }

    TSharedPtr<FJsonObject> InfoPayload = MakeShared<FJsonObject>();
    InfoPayload->SetStringField(TEXT("assetPath"), ToObjectPath(PackagePath));
    FTestResponseCapture InfoCapture;
    TestTrue(TEXT("gas.get_gas_info handler found"),
        InvokeHandlerWithCapture(TEXT("gas.get_gas_info"), InfoPayload, InfoCapture));
    if (!TestTrue(TEXT("gas.get_gas_info succeeded"), InfoCapture.bSuccess))
    {
        AddError(FString::Printf(TEXT("get_gas_info failed: %s %s"),
            *InfoCapture.ErrorCode, *InfoCapture.Message));
        return true;
    }

    // History #2: the GameplayEffect branch must surface the wired execution calc
    // and the granted owned tag, not just modifier/cue counts.
    int32 ExecutionCount = 0;
    InfoCapture.Result->TryGetNumberField(TEXT("executionCount"), ExecutionCount);
    TestEqual(TEXT("executionCount reflects the wired execution calc"), ExecutionCount, 1);

    TestTrue(TEXT("grantedTags lists the configured owned tag"),
        JsonStringArrayContains(InfoCapture.Result, TEXT("grantedTags"), OwnedTagName));

    return true;
#endif
}

// ============================================================================
// gas.get_gas_info — GameplayAbility branch echoes the wired cooldown / cost GE
// classes and the ability tags
// Regression for E-gas-info-ability-omits-cooldown-cost-tags.
//
// The defect: the GameplayAbility branch (GASHandler.cpp ~:2691-2720) stamped only
// gasType / instancingPolicy / netExecutionPolicy and never read the ability CDO's
// CooldownGameplayEffectClass / CostGameplayEffectClass / AbilityTags — so the named
// "read back the GAS info to confirm the cooldown and cost are wired" success-check
// was unsatisfiable via the tool and the agent had to pivot to property.get on the
// CDO. The fix emits cooldownEffect / costEffect (the CDO class object paths) and an
// abilityTags array from the same fields gas.set_ability_cooldown / set_ability_costs
// / set_ability_tags write.
//
// Counterfactual: revert the three field emits and cooldownEffect/costEffect are
// absent (the path-match assertions read an empty string) and abilityTags is absent
// (its membership assertion fails) — exactly the readback gap the ticket describes.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGasGetGasInfoAbilityEmitsCooldownCostTagsTest,
    "PinWright.gas.get_gas_info.AbilityEmitsCooldownCostTags",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGasGetGasInfoAbilityEmitsCooldownCostTagsTest::RunTest(const FString& Parameters)
{
#if !MCP_GAS_TEST_HAS_GAS
    PinWrightTestSkip::SkipAssertions(*this, TEXT("optional-plugin-not-shipped"),
        TEXT("GameplayAbilities plugin not available; skipping."));
    return true;
#else
    // Build a GameplayAbility plus a cooldown and a cost GameplayEffect to wire.
    const FString AbilityPath = CreateAbility(*this, TEXT("GA_InfoCooldownCost"));
    if (AbilityPath.IsEmpty())
    {
        return true;
    }
    ON_SCOPE_EXIT { CleanupTestAsset(AbilityPath); };

    const FString CooldownPath = CreateEffect(*this, TEXT("GE_InfoCooldown"), TEXT("has_duration"));
    if (CooldownPath.IsEmpty())
    {
        return true;
    }
    ON_SCOPE_EXIT { CleanupTestAsset(CooldownPath); };

    const FString CostPath = CreateEffect(*this, TEXT("GE_InfoCost"), TEXT("instant"));
    if (CostPath.IsEmpty())
    {
        return true;
    }
    ON_SCOPE_EXIT { CleanupTestAsset(CostPath); };

    const FString AbilityObjectPath = ToObjectPath(AbilityPath);

    // Wire the cooldown + cost GE classes via the canonical setters (the same write
    // path the ticket's task used). get_gas_info must read these back.
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("blueprintPath"), AbilityObjectPath);
        Payload->SetStringField(TEXT("cooldownEffectPath"), ToObjectPath(CooldownPath));
        FTestResponseCapture Capture;
        TestTrue(TEXT("gas.set_ability_cooldown handler found"),
            InvokeHandlerWithCapture(TEXT("gas.set_ability_cooldown"), Payload, Capture));
        if (!TestTrue(TEXT("set_ability_cooldown succeeded"), Capture.bSuccess))
        {
            AddError(FString::Printf(TEXT("set_ability_cooldown failed: %s %s"),
                *Capture.ErrorCode, *Capture.Message));
            return true;
        }
    }
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("blueprintPath"), AbilityObjectPath);
        Payload->SetStringField(TEXT("costEffectPath"), ToObjectPath(CostPath));
        FTestResponseCapture Capture;
        TestTrue(TEXT("gas.set_ability_costs handler found"),
            InvokeHandlerWithCapture(TEXT("gas.set_ability_costs"), Payload, Capture));
        if (!TestTrue(TEXT("set_ability_costs succeeded"), Capture.bSuccess))
        {
            AddError(FString::Printf(TEXT("set_ability_costs failed: %s %s"),
                *Capture.ErrorCode, *Capture.Message));
            return true;
        }
    }

    // Register an ability tag transiently (set_ability_tags drops unregistered tags)
    // and apply it.
    const FString AbilityTagName = TEXT("PinWrightTest.Ability.Fireball");
    if (IGameplayTagsEditorModule::IsAvailable())
    {
        IGameplayTagsEditorModule::Get().AddTransientEditorGameplayTag(AbilityTagName);
    }
    if (!TestTrue(TEXT("ability tag is registered"),
            UGameplayTagsManager::Get().RequestGameplayTag(FName(*AbilityTagName), false).IsValid()))
    {
        return true;
    }
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("blueprintPath"), AbilityObjectPath);
        TArray<TSharedPtr<FJsonValue>> TagsArr;
        TagsArr.Add(MakeShared<FJsonValueString>(AbilityTagName));
        Payload->SetArrayField(TEXT("abilityTags"), TagsArr);
        FTestResponseCapture Capture;
        TestTrue(TEXT("gas.set_ability_tags handler found"),
            InvokeHandlerWithCapture(TEXT("gas.set_ability_tags"), Payload, Capture));
        if (!TestTrue(TEXT("set_ability_tags succeeded"), Capture.bSuccess))
        {
            AddError(FString::Printf(TEXT("set_ability_tags failed: %s %s"),
                *Capture.ErrorCode, *Capture.Message));
            return true;
        }
    }

    // The expected persisted generated classes — get_gas_info emits their object
    // path (GetPathName(), e.g. /Game/.../GE_X.GE_X_C).
    UClass* CooldownGenClass = LoadClass<UGameplayEffect>(nullptr, *(ToObjectPath(CooldownPath) + TEXT("_C")));
    UClass* CostGenClass = LoadClass<UGameplayEffect>(nullptr, *(ToObjectPath(CostPath) + TEXT("_C")));
    if (!TestNotNull(TEXT("cooldown GE generated class loads"), CooldownGenClass)
        || !TestNotNull(TEXT("cost GE generated class loads"), CostGenClass))
    {
        return true;
    }

    // get_gas_info must now surface all three.
    TSharedPtr<FJsonObject> InfoPayload = MakeShared<FJsonObject>();
    InfoPayload->SetStringField(TEXT("assetPath"), AbilityObjectPath);
    FTestResponseCapture InfoCapture;
    TestTrue(TEXT("gas.get_gas_info handler found"),
        InvokeHandlerWithCapture(TEXT("gas.get_gas_info"), InfoPayload, InfoCapture));
    if (!TestTrue(TEXT("gas.get_gas_info succeeded"), InfoCapture.bSuccess))
    {
        AddError(FString::Printf(TEXT("get_gas_info failed: %s %s"),
            *InfoCapture.ErrorCode, *InfoCapture.Message));
        return true;
    }

    // The branch was reached as a GameplayAbility (guards the assertions below).
    FString GasType;
    InfoCapture.Result->TryGetStringField(TEXT("gasType"), GasType);
    if (!TestEqual(TEXT("gasType is GameplayAbility"), GasType, FString(TEXT("GameplayAbility"))))
    {
        return true;
    }

    // Load-bearing: cooldownEffect / costEffect echo the persisted CDO class paths.
    FString CooldownEffect;
    InfoCapture.Result->TryGetStringField(TEXT("cooldownEffect"), CooldownEffect);
    TestEqual(TEXT("cooldownEffect echoes the wired cooldown GE class path"),
        CooldownEffect, CooldownGenClass->GetPathName());

    FString CostEffect;
    InfoCapture.Result->TryGetStringField(TEXT("costEffect"), CostEffect);
    TestEqual(TEXT("costEffect echoes the wired cost GE class path"),
        CostEffect, CostGenClass->GetPathName());

    // Load-bearing: abilityTags lists the tag set by set_ability_tags.
    TestTrue(TEXT("abilityTags lists the configured ability tag"),
        JsonStringArrayContains(InfoCapture.Result, TEXT("abilityTags"), AbilityTagName));

    return true;
#endif
}

// ============================================================================
// gas.set_ability_tags — the three activation-gating containers round-trip
// Regression for F-gas-ability-activation-tags-unauthorable.
//
// The gap: gas.set_ability_tags exposed only abilityTags/cancel/block, so the
// owner-state activation containers (ActivationBlockedTags / ActivationRequiredTags
// / ActivationOwnedTags — "can't cast while stunned") had no dedicated param, and
// the nearest one (blockAbilitiesWithTags -> BlockAbilitiesWithTag) is inverted, so
// the obvious verb produced a silently-ungated ability. The fix adds
// activationBlockedTags/activationRequiredTags/activationOwnedTags params (validated
// via ResolveTagsInto, written via the generic AddTagToAbilityContainer reflection
// helper) and surfaces the three containers in get_gas_info.
//
// This routes one registered tag into each activation param, then reads them back
// through get_gas_info — exercising BOTH halves via production code: revert the
// write loops and the containers stay empty -> the readback arrays are empty -> the
// membership assertions miss; revert the get_gas_info emits and the arrays are
// absent -> the membership assertions miss. The blocked-tag assertion also proves
// "blocked while stunned" landed in the correct native container (ActivationBlockedTags),
// not the inverted BlockAbilitiesWithTag the old forced workaround used.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGasSetAbilityActivationTagsRoundTripTest,
    "PinWright.gas.set_ability_tags.ActivationTagsAuthoredAndReadBack",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGasSetAbilityActivationTagsRoundTripTest::RunTest(const FString& Parameters)
{
#if !MCP_GAS_TEST_HAS_GAS
    PinWrightTestSkip::SkipAssertions(*this, TEXT("optional-plugin-not-shipped"),
        TEXT("GameplayAbilities plugin not available; skipping."));
    return true;
#else
    const FString AbilityPath = CreateAbility(*this, TEXT("GA_ActivationTags"));
    if (AbilityPath.IsEmpty())
    {
        return true;
    }
    ON_SCOPE_EXIT { CleanupTestAsset(AbilityPath); };

    const FString AbilityObjectPath = ToObjectPath(AbilityPath);

    // One registered tag per activation container. set_ability_tags rejects
    // unregistered tags (droppedTags), so register all three transiently first.
    const FString BlockedTag  = TEXT("PinWrightTest.State.Stunned");    // -> ActivationBlockedTags
    const FString RequiredTag = TEXT("PinWrightTest.State.Grounded");   // -> ActivationRequiredTags
    const FString OwnedTag    = TEXT("PinWrightTest.State.Casting");    // -> ActivationOwnedTags
    if (IGameplayTagsEditorModule::IsAvailable())
    {
        IGameplayTagsEditorModule::Get().AddTransientEditorGameplayTag(BlockedTag);
        IGameplayTagsEditorModule::Get().AddTransientEditorGameplayTag(RequiredTag);
        IGameplayTagsEditorModule::Get().AddTransientEditorGameplayTag(OwnedTag);
    }
    if (!TestTrue(TEXT("activation tags registered"),
            UGameplayTagsManager::Get().RequestGameplayTag(FName(*BlockedTag), false).IsValid()
            && UGameplayTagsManager::Get().RequestGameplayTag(FName(*RequiredTag), false).IsValid()
            && UGameplayTagsManager::Get().RequestGameplayTag(FName(*OwnedTag), false).IsValid()))
    {
        return true;
    }

    // Author all three activation containers in one call — the capability the ticket
    // is about (previously reachable only via a source-dive blueprint.set_default).
    {
        auto SingleTagArray = [](const FString& Tag)
        {
            TArray<TSharedPtr<FJsonValue>> Arr;
            Arr.Add(MakeShared<FJsonValueString>(Tag));
            return Arr;
        };
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("blueprintPath"), AbilityObjectPath);
        Payload->SetArrayField(TEXT("activationBlockedTags"), SingleTagArray(BlockedTag));
        Payload->SetArrayField(TEXT("activationRequiredTags"), SingleTagArray(RequiredTag));
        Payload->SetArrayField(TEXT("activationOwnedTags"), SingleTagArray(OwnedTag));
        FTestResponseCapture Capture;
        TestTrue(TEXT("gas.set_ability_tags handler found"),
            InvokeHandlerWithCapture(TEXT("gas.set_ability_tags"), Payload, Capture));
        if (!TestTrue(TEXT("set_ability_tags succeeded"), Capture.bSuccess))
        {
            AddError(FString::Printf(TEXT("set_ability_tags failed: %s %s"),
                *Capture.ErrorCode, *Capture.Message));
            return true;
        }
    }

    // get_gas_info must now echo each container.
    TSharedPtr<FJsonObject> InfoPayload = MakeShared<FJsonObject>();
    InfoPayload->SetStringField(TEXT("assetPath"), AbilityObjectPath);
    FTestResponseCapture InfoCapture;
    TestTrue(TEXT("gas.get_gas_info handler found"),
        InvokeHandlerWithCapture(TEXT("gas.get_gas_info"), InfoPayload, InfoCapture));
    if (!TestTrue(TEXT("gas.get_gas_info succeeded"), InfoCapture.bSuccess))
    {
        AddError(FString::Printf(TEXT("get_gas_info failed: %s %s"),
            *InfoCapture.ErrorCode, *InfoCapture.Message));
        return true;
    }

    FString GasType;
    InfoCapture.Result->TryGetStringField(TEXT("gasType"), GasType);
    if (!TestEqual(TEXT("gasType is GameplayAbility"), GasType, FString(TEXT("GameplayAbility"))))
    {
        return true;
    }

    // Load-bearing: each activation container round-trips into its own get_gas_info
    // array. Reverting either the write params or the readback emits fails these.
    TestTrue(TEXT("activationBlockedTags echoes the stun-block tag"),
        JsonStringArrayContains(InfoCapture.Result, TEXT("activationBlockedTags"), BlockedTag));
    TestTrue(TEXT("activationRequiredTags echoes the required tag"),
        JsonStringArrayContains(InfoCapture.Result, TEXT("activationRequiredTags"), RequiredTag));
    TestTrue(TEXT("activationOwnedTags echoes the owned tag"),
        JsonStringArrayContains(InfoCapture.Result, TEXT("activationOwnedTags"), OwnedTag));

    return true;
#endif
}

// ============================================================================
// gas.get_gas_info — GameplayEffect branch echoes per-modifier op/magnitude/attribute
// + the duration value, and the AttributeSet branch echoes the attributes list
// Regression for E-gas-info-effect-modifiers-duration-value-thin.
//
// The defect: the GameplayEffect branch emitted only modifierCount (never each
// modifier's op/magnitude/attribute) and durationPolicy as a raw enum int (never the
// DurationMagnitude value written by set_effect_duration), and the AttributeSet branch
// emitted only gasType:"AttributeSet" (never the attributes). So a "confirm the
// duration AND both modifiers' ops and magnitudes" / "confirm all attributes and their
// defaults" readback forced an asset.dump -> properties.json pivot. The fix widens both
// branches: durationMagnitude + durationPolicyName + a modifiers array on the GE branch,
// and an attributes array on the AttributeSet branch.
//
// Counterfactual: revert any of those emits and the matching assertion below fails
// (durationMagnitude/durationPolicyName absent; modifiers array empty so the op/mag/attr
// assertions miss; attributes array absent so the name/baseValue assertions miss).
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGasGetGasInfoModifiersDurationAndAttributesTest,
    "PinWright.gas.get_gas_info.EffectModifiersDurationAndAttributeSetAttributes",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGasGetGasInfoModifiersDurationAndAttributesTest::RunTest(const FString& Parameters)
{
#if !MCP_GAS_TEST_HAS_GAS
    PinWrightTestSkip::SkipAssertions(*this, TEXT("optional-plugin-not-shipped"),
        TEXT("GameplayAbilities plugin not available; skipping."));
    return true;
#else
    // 1. Build an AttributeSet with AttackPower (gas.add_attribute compiles it onto the
    //    generated class) and give it a base value of 12 via set_attribute_base_value.
    const FString AttrSetPath = CreateAttributeSet(*this, TEXT("AS_InfoReadback"));
    if (AttrSetPath.IsEmpty())
    {
        return true;
    }
    ON_SCOPE_EXIT { CleanupTestAsset(AttrSetPath); };
    const FString AttrSetObjectPath = ToObjectPath(AttrSetPath);
    {
        TSharedPtr<FJsonObject> AddPayload = MakeShared<FJsonObject>();
        AddPayload->SetStringField(TEXT("blueprintPath"), AttrSetObjectPath);
        AddPayload->SetStringField(TEXT("attributeName"), TEXT("AttackPower"));
        FTestResponseCapture AddCapture;
        TestTrue(TEXT("gas.add_attribute handler found"),
            InvokeHandlerWithCapture(TEXT("gas.add_attribute"), AddPayload, AddCapture));
        if (!TestTrue(TEXT("gas.add_attribute succeeded"), AddCapture.bSuccess))
        {
            AddError(FString::Printf(TEXT("add_attribute failed: %s %s"),
                *AddCapture.ErrorCode, *AddCapture.Message));
            return true;
        }
    }
    {
        TSharedPtr<FJsonObject> BasePayload = MakeShared<FJsonObject>();
        BasePayload->SetStringField(TEXT("blueprintPath"), AttrSetObjectPath);
        BasePayload->SetStringField(TEXT("attributeName"), TEXT("AttackPower"));
        BasePayload->SetNumberField(TEXT("baseValue"), 12.0);
        FTestResponseCapture BaseCapture;
        TestTrue(TEXT("gas.set_attribute_base_value handler found"),
            InvokeHandlerWithCapture(TEXT("gas.set_attribute_base_value"), BasePayload, BaseCapture));
        TestTrue(TEXT("gas.set_attribute_base_value succeeded"), BaseCapture.bSuccess);
    }

    // The attribute spec the modifier binds to: '<generated class>.AttrName'.
    const FString AttrSetName = FPackageName::GetLongPackageAssetName(AttrSetPath);
    const FString AttrSpec = FString::Printf(TEXT("%s.%s_C.AttackPower"), *AttrSetPath, *AttrSetName);

    // 2. Build a has_duration GameplayEffect, set duration 8s, add two additive
    //    modifiers (+25 bound to AttackPower, -10 unbound), then bump idx 0 to 30 —
    //    the exact authoring run the ticket's story describes.
    const FString EffectPath = CreateEffect(*this, TEXT("GE_InfoReadback"), TEXT("has_duration"));
    if (EffectPath.IsEmpty())
    {
        return true;
    }
    ON_SCOPE_EXIT { CleanupTestAsset(EffectPath); };
    const FString EffectObjectPath = ToObjectPath(EffectPath);
    {
        TSharedPtr<FJsonObject> DurPayload = MakeShared<FJsonObject>();
        DurPayload->SetStringField(TEXT("blueprintPath"), EffectObjectPath);
        DurPayload->SetStringField(TEXT("durationType"), TEXT("has_duration"));
        DurPayload->SetNumberField(TEXT("duration"), 8.0);
        FTestResponseCapture DurCapture;
        TestTrue(TEXT("gas.set_effect_duration handler found"),
            InvokeHandlerWithCapture(TEXT("gas.set_effect_duration"), DurPayload, DurCapture));
        TestTrue(TEXT("gas.set_effect_duration succeeded"), DurCapture.bSuccess);
    }
    {
        TSharedPtr<FJsonObject> ModPayload = MakeShared<FJsonObject>();
        ModPayload->SetStringField(TEXT("blueprintPath"), EffectObjectPath);
        ModPayload->SetStringField(TEXT("operation"), TEXT("additive"));
        ModPayload->SetNumberField(TEXT("magnitude"), 25.0);
        ModPayload->SetStringField(TEXT("attribute"), AttrSpec);
        FTestResponseCapture ModCapture;
        TestTrue(TEXT("gas.add_effect_modifier (idx0) handler found"),
            InvokeHandlerWithCapture(TEXT("gas.add_effect_modifier"), ModPayload, ModCapture));
        if (!TestTrue(TEXT("gas.add_effect_modifier (idx0) succeeded"), ModCapture.bSuccess))
        {
            AddError(FString::Printf(TEXT("add_effect_modifier idx0 failed: %s %s"),
                *ModCapture.ErrorCode, *ModCapture.Message));
            return true;
        }
    }
    {
        TSharedPtr<FJsonObject> ModPayload = MakeShared<FJsonObject>();
        ModPayload->SetStringField(TEXT("blueprintPath"), EffectObjectPath);
        ModPayload->SetStringField(TEXT("operation"), TEXT("additive"));
        ModPayload->SetNumberField(TEXT("magnitude"), -10.0);
        FTestResponseCapture ModCapture;
        TestTrue(TEXT("gas.add_effect_modifier (idx1) handler found"),
            InvokeHandlerWithCapture(TEXT("gas.add_effect_modifier"), ModPayload, ModCapture));
        TestTrue(TEXT("gas.add_effect_modifier (idx1) succeeded"), ModCapture.bSuccess);
    }
    {
        TSharedPtr<FJsonObject> SetPayload = MakeShared<FJsonObject>();
        SetPayload->SetStringField(TEXT("blueprintPath"), EffectObjectPath);
        SetPayload->SetNumberField(TEXT("modifierIndex"), 0);
        SetPayload->SetNumberField(TEXT("value"), 30.0);
        FTestResponseCapture SetCapture;
        TestTrue(TEXT("gas.set_modifier_magnitude handler found"),
            InvokeHandlerWithCapture(TEXT("gas.set_modifier_magnitude"), SetPayload, SetCapture));
        TestTrue(TEXT("gas.set_modifier_magnitude succeeded"), SetCapture.bSuccess);
    }

    // 3. get_gas_info on the GameplayEffect must now read back the duration VALUE,
    //    a readable policy name, and per-modifier op/magnitude/attribute.
    {
        TSharedPtr<FJsonObject> InfoPayload = MakeShared<FJsonObject>();
        InfoPayload->SetStringField(TEXT("assetPath"), EffectObjectPath);
        FTestResponseCapture InfoCapture;
        TestTrue(TEXT("gas.get_gas_info (GE) handler found"),
            InvokeHandlerWithCapture(TEXT("gas.get_gas_info"), InfoPayload, InfoCapture));
        if (!TestTrue(TEXT("gas.get_gas_info (GE) succeeded"), InfoCapture.bSuccess))
        {
            AddError(FString::Printf(TEXT("get_gas_info (GE) failed: %s %s"),
                *InfoCapture.ErrorCode, *InfoCapture.Message));
            return true;
        }

        FString PolicyName;
        InfoCapture.Result->TryGetStringField(TEXT("durationPolicyName"), PolicyName);
        TestEqual(TEXT("durationPolicyName decodes the enum"), PolicyName, FString(TEXT("HasDuration")));

        double DurationMag = 0.0;
        TestTrue(TEXT("durationMagnitude is present"),
            InfoCapture.Result->TryGetNumberField(TEXT("durationMagnitude"), DurationMag));
        TestTrue(TEXT("durationMagnitude is the 8s written by set_effect_duration"),
            FMath::IsNearlyEqual(static_cast<float>(DurationMag), 8.0f));

        const TArray<TSharedPtr<FJsonValue>>* Modifiers = nullptr;
        if (TestTrue(TEXT("modifiers array is present"),
                InfoCapture.Result->TryGetArrayField(TEXT("modifiers"), Modifiers))
            && TestEqual(TEXT("modifiers array has two entries"), Modifiers->Num(), 2))
        {
            const TSharedPtr<FJsonObject> Mod0 = (*Modifiers)[0]->AsObject();
            const TSharedPtr<FJsonObject> Mod1 = (*Modifiers)[1]->AsObject();
            if (TestTrue(TEXT("modifier entries are objects"), Mod0.IsValid() && Mod1.IsValid()))
            {
                // idx0: additive, bumped to 30, bound to AttackPower. The op string is
                // the engine's own name for the Additive op (== AddBase, value 0) — read
                // it from the same function the handler uses so the assertion can't drift.
                const FString ExpectedAddOp =
                    EGameplayModOpToString(static_cast<int32>(EGameplayModOp::Additive));
                FString Op0;
                Mod0->TryGetStringField(TEXT("operation"), Op0);
                TestEqual(TEXT("modifier[0] operation is the additive op name"),
                    Op0, ExpectedAddOp);
                TestFalse(TEXT("modifier[0] operation is non-empty"), Op0.IsEmpty());
                double Mag0 = 0.0;
                Mod0->TryGetNumberField(TEXT("magnitude"), Mag0);
                TestTrue(TEXT("modifier[0] magnitude is the bumped 30"),
                    FMath::IsNearlyEqual(static_cast<float>(Mag0), 30.0f));
                FString Attr0;
                Mod0->TryGetStringField(TEXT("attribute"), Attr0);
                TestEqual(TEXT("modifier[0] attribute is the bound AttackPower"),
                    Attr0, FString(TEXT("AttackPower")));

                // idx1: additive -10, unbound (empty attribute).
                double Mag1 = 0.0;
                Mod1->TryGetNumberField(TEXT("magnitude"), Mag1);
                TestTrue(TEXT("modifier[1] magnitude is -10"),
                    FMath::IsNearlyEqual(static_cast<float>(Mag1), -10.0f));
                FString Attr1;
                Mod1->TryGetStringField(TEXT("attribute"), Attr1);
                TestEqual(TEXT("modifier[1] attribute is empty (unbound)"), Attr1, FString());
            }
        }
    }

    // 4. get_gas_info on the AttributeSet must read back the attributes list with
    //    the AttackPower name + its baseValue (12).
    {
        TSharedPtr<FJsonObject> InfoPayload = MakeShared<FJsonObject>();
        InfoPayload->SetStringField(TEXT("assetPath"), AttrSetObjectPath);
        FTestResponseCapture InfoCapture;
        TestTrue(TEXT("gas.get_gas_info (AttributeSet) handler found"),
            InvokeHandlerWithCapture(TEXT("gas.get_gas_info"), InfoPayload, InfoCapture));
        if (!TestTrue(TEXT("gas.get_gas_info (AttributeSet) succeeded"), InfoCapture.bSuccess))
        {
            AddError(FString::Printf(TEXT("get_gas_info (AttributeSet) failed: %s %s"),
                *InfoCapture.ErrorCode, *InfoCapture.Message));
            return true;
        }

        FString GasType;
        InfoCapture.Result->TryGetStringField(TEXT("gasType"), GasType);
        TestEqual(TEXT("gasType is AttributeSet"), GasType, FString(TEXT("AttributeSet")));

        const TArray<TSharedPtr<FJsonValue>>* Attributes = nullptr;
        if (TestTrue(TEXT("attributes array is present"),
                InfoCapture.Result->TryGetArrayField(TEXT("attributes"), Attributes))
            && TestTrue(TEXT("attributes array is non-empty"), Attributes && Attributes->Num() > 0))
        {
            // Find the AttackPower entry (robust to any other inherited attributes).
            bool bFound = false;
            for (const TSharedPtr<FJsonValue>& Value : *Attributes)
            {
                const TSharedPtr<FJsonObject> Entry = Value.IsValid() ? Value->AsObject() : nullptr;
                if (!Entry.IsValid())
                {
                    continue;
                }
                FString Name;
                Entry->TryGetStringField(TEXT("name"), Name);
                if (Name == TEXT("AttackPower"))
                {
                    bFound = true;
                    double BaseValue = 0.0;
                    Entry->TryGetNumberField(TEXT("baseValue"), BaseValue);
                    TestTrue(TEXT("AttackPower baseValue is the 12 written by set_attribute_base_value"),
                        FMath::IsNearlyEqual(static_cast<float>(BaseValue), 12.0f));
                    break;
                }
            }
            TestTrue(TEXT("attributes array names AttackPower"), bFound);
        }
    }

    return true;
#endif
}

// ============================================================================
// gas.set_ability_cooldown / gas.set_ability_costs — a PLAIN blueprint-asset
// path persists the GameplayEffect class to the CDO, and a non-resolving path is
// REJECTED rather than reported as a clean success
// (B-set-ability-cooldown-cost-class-path-silent-drop).
//
// Before the fix both setters resolved the effect path with
// LoadClass<UGameplayEffect> gated on a bare `if (Class)` with no else, then fell
// through to SendSuccess regardless. LoadClass only resolves the generated-class
// path (/Game/.../GE_X.GE_X_C); the plain asset path (/Game/.../GE_X) returned
// null, the write was silently skipped, and the handler still echoed the path as
// if it took. The fix routes both paths through ResolveUClass (which accepts the
// plain asset path via the _C suffix / GeneratedClass fallback) and rejects
// NOT_FOUND when nothing resolves.
//
// Counterfactual: revert to LoadClass + bare-if + unconditional SendSuccess and
//   (a) the plain-path readback below reads null (the persisted-class assertion
//       fails), and
//   (b) the bogus-path call returns ok:true (the NOT_FOUND-rejection assertions
//       fail because the old code silent-successes on an unresolved path).
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGasSetAbilityCooldownCostPlainPathPersistsTest,
    "PinWright.gas.set_ability_cooldown_costs.PlainPathPersistsAndRejectsMissing",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGasSetAbilityCooldownCostPlainPathPersistsTest::RunTest(const FString& Parameters)
{
#if !MCP_GAS_TEST_HAS_GAS
    PinWrightTestSkip::SkipAssertions(*this, TEXT("optional-plugin-not-shipped"),
        TEXT("GameplayAbilities plugin not available; skipping."));
    return true;
#else
    // Build a GameplayAbility and two GameplayEffects (cooldown + cost).
    const FString AbilityPath = CreateAbility(*this, TEXT("GA_CdCostProbe"));
    if (AbilityPath.IsEmpty())
    {
        return true;
    }
    ON_SCOPE_EXIT { CleanupTestAsset(AbilityPath); };

    const FString CooldownPath = CreateEffect(*this, TEXT("GE_Cooldown"), TEXT("has_duration"));
    if (CooldownPath.IsEmpty())
    {
        return true;
    }
    ON_SCOPE_EXIT { CleanupTestAsset(CooldownPath); };

    const FString CostPath = CreateEffect(*this, TEXT("GE_Cost"), TEXT("instant"));
    if (CostPath.IsEmpty())
    {
        return true;
    }
    ON_SCOPE_EXIT { CleanupTestAsset(CostPath); };

    const FString AbilityObjectPath = ToObjectPath(AbilityPath);

    // The expected resolved generated classes for the readback assertions.
    UClass* CooldownGenClass = LoadClass<UGameplayEffect>(nullptr, *(ToObjectPath(CooldownPath) + TEXT("_C")));
    UClass* CostGenClass = LoadClass<UGameplayEffect>(nullptr, *(ToObjectPath(CostPath) + TEXT("_C")));
    if (!TestNotNull(TEXT("cooldown GE generated class loads"), CooldownGenClass)
        || !TestNotNull(TEXT("cost GE generated class loads"), CostGenClass))
    {
        return true;
    }

    // 1. set_ability_cooldown with the PLAIN blueprint-asset path (no _C suffix).
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("blueprintPath"), AbilityObjectPath);
        Payload->SetStringField(TEXT("cooldownEffectPath"), ToObjectPath(CooldownPath));
        FTestResponseCapture Capture;
        TestTrue(TEXT("gas.set_ability_cooldown handler found"),
            InvokeHandlerWithCapture(TEXT("gas.set_ability_cooldown"), Payload, Capture));
        if (!TestTrue(TEXT("set_ability_cooldown succeeds on a plain asset path"), Capture.bSuccess))
        {
            AddError(FString::Printf(TEXT("set_ability_cooldown failed: %s %s"),
                *Capture.ErrorCode, *Capture.Message));
            return true;
        }
    }

    // 2. set_ability_costs with the PLAIN blueprint-asset path.
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("blueprintPath"), AbilityObjectPath);
        Payload->SetStringField(TEXT("costEffectPath"), ToObjectPath(CostPath));
        FTestResponseCapture Capture;
        TestTrue(TEXT("gas.set_ability_costs handler found"),
            InvokeHandlerWithCapture(TEXT("gas.set_ability_costs"), Payload, Capture));
        if (!TestTrue(TEXT("set_ability_costs succeeds on a plain asset path"), Capture.bSuccess))
        {
            AddError(FString::Printf(TEXT("set_ability_costs failed: %s %s"),
                *Capture.ErrorCode, *Capture.Message));
            return true;
        }
    }

    // 3. The load-bearing assertion: both classes are actually PERSISTED on the
    //    ability CDO (the plain-path readback the ticket showed reading null).
    UBlueprint* AbilityBP = Cast<UBlueprint>(UEditorAssetLibrary::LoadAsset(AbilityObjectPath));
    if (!TestNotNull(TEXT("ability blueprint loads"), AbilityBP)
        || !TestNotNull(TEXT("ability generated class exists"),
                AbilityBP ? AbilityBP->GeneratedClass.Get() : nullptr))
    {
        return true;
    }
    UObject* AbilityCDO = AbilityBP->GeneratedClass->GetDefaultObject();
    if (!TestNotNull(TEXT("ability CDO exists"), AbilityCDO))
    {
        return true;
    }

    const auto ReadClassProperty = [AbilityCDO](const TCHAR* PropName) -> UClass*
    {
        FProperty* Prop = AbilityCDO->GetClass()->FindPropertyByName(FName(PropName));
        FClassProperty* ClassProp = CastField<FClassProperty>(Prop);
        if (!ClassProp)
        {
            return nullptr;
        }
        UObject* Value = ClassProp->GetObjectPropertyValue_InContainer(AbilityCDO);
        return Cast<UClass>(Value);
    };

    TestEqual(TEXT("CooldownGameplayEffectClass persisted from the plain path"),
        ReadClassProperty(TEXT("CooldownGameplayEffectClass")), CooldownGenClass);
    TestEqual(TEXT("CostGameplayEffectClass persisted from the plain path"),
        ReadClassProperty(TEXT("CostGameplayEffectClass")), CostGenClass);

    // 4 & 5. A non-resolving effect path must be REJECTED (NOT_FOUND) by BOTH
    //    setters, not reported as a clean silent success. One contract, asserted
    //    per setter via its method + effect-field pair.
    const auto ExpectRejectsMissing = [&](const TCHAR* Method, const TCHAR* EffectField)
    {
        const FString MissingPath = FString::Printf(
            TEXT("/Game/__PW_GatewayTests/GE_DoesNotExist_%s"),
            *FGuid::NewGuid().ToString(EGuidFormats::Digits));
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("blueprintPath"), AbilityObjectPath);
        Payload->SetStringField(EffectField, MissingPath);
        FTestResponseCapture Capture;
        TestTrue(FString::Printf(TEXT("%s handler found (missing path)"), Method),
            InvokeHandlerWithCapture(Method, Payload, Capture));
        TestFalse(FString::Printf(TEXT("%s rejects a non-resolving path (no silent success)"), Method),
            Capture.bSuccess);
        TestEqual(FString::Printf(TEXT("%s error code is NOT_FOUND"), Method),
            Capture.ErrorCode, FString(TEXT("NOT_FOUND")));
    };

    ExpectRejectsMissing(TEXT("gas.set_ability_cooldown"), TEXT("cooldownEffectPath"));
    ExpectRejectsMissing(TEXT("gas.set_ability_costs"), TEXT("costEffectPath"));

    return true;
#endif
}

// ============================================================================
// gas.set_effect_tags grants tags through the UE 5.3+ component model, so the
// engine's cooldown-GE validator (which reads GetGrantedTags()) actually sees them
// (E-gas-set-effect-tags-writes-deprecated-container-not-component).
//
// The defect: the handler wrote tags only into the deprecated
// InheritableOwnedTagsContainer, never adding a UTargetTagsGameplayEffectComponent
// to GEComponents. For a freshly created GE the deprecated field is never migrated
// into a component (the engine's one-time pre-Modular53 upgrade does not run), so
// GetGrantedTags() — what the cooldown-GE validator checks — stays EMPTY even though
// set_effect_tags echoed tagsAdded and get_gas_info reported grantedTags (both read
// the deprecated container). The fix writes the component model.
//
// Load-bearing assertion: EffectCDO->GetGrantedTags() (== CachedGrantedTags, rebuilt
// from GEComponents — the exact source the cooldown validator reads) must contain the
// granted tag after set_effect_tags. Counterfactual: revert the handler to the
// deprecated-only AddTag and GEComponents stays empty → GetGrantedTags() is empty →
// the "live CDO grants the tag" assertion fails. We also assert get_gas_info's
// grantedTags now reflects the component model so the readback matches the CDO.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGasSetEffectTagsGrantsViaComponentTest,
    "PinWright.gas.set_effect_tags.GrantsViaComponent",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGasSetEffectTagsGrantsViaComponentTest::RunTest(const FString& Parameters)
{
#if !MCP_GAS_TEST_HAS_GAS
    PinWrightTestSkip::SkipAssertions(*this, TEXT("optional-plugin-not-shipped"),
        TEXT("GameplayAbilities plugin not available; skipping."));
    return true;
#else
    // A has_duration GE — the realistic shape for a cooldown GE that must grant tags.
    const FString PackagePath = CreateEffect(*this, TEXT("GE_CooldownGrant"), TEXT("has_duration"));
    if (PackagePath.IsEmpty())
    {
        return true;
    }
    ON_SCOPE_EXIT { CleanupTestAsset(PackagePath); };

    // Register the cooldown tag transiently for this session so the validate-before-
    // mutate gate passes (this is the "tag WAS registered" scenario the ticket calls
    // out, distinct from the unregistered-drop case).
    const FString CooldownTag = TEXT("PinWrightTest.Cooldown.Fireball");
    if (IGameplayTagsEditorModule::IsAvailable())
    {
        IGameplayTagsEditorModule::Get().AddTransientEditorGameplayTag(CooldownTag);
    }
    if (!TestTrue(TEXT("cooldown tag is registered"),
            UGameplayTagsManager::Get().RequestGameplayTag(FName(*CooldownTag), false).IsValid()))
    {
        return true;
    }

    {
        TSharedPtr<FJsonObject> TagPayload = MakeShared<FJsonObject>();
        TagPayload->SetStringField(TEXT("blueprintPath"), ToObjectPath(PackagePath));
        TArray<TSharedPtr<FJsonValue>> TagsArr;
        TagsArr.Add(MakeShared<FJsonValueString>(CooldownTag));
        TagPayload->SetArrayField(TEXT("grantedTags"), TagsArr);
        FTestResponseCapture TagCapture;
        TestTrue(TEXT("gas.set_effect_tags handler found"),
            InvokeHandlerWithCapture(TEXT("gas.set_effect_tags"), TagPayload, TagCapture));
        if (!TestTrue(TEXT("gas.set_effect_tags succeeded"), TagCapture.bSuccess))
        {
            AddError(FString::Printf(TEXT("set_effect_tags failed: %s %s"),
                *TagCapture.ErrorCode, *TagCapture.Message));
            return true;
        }
    }

    UBlueprint* Blueprint = Cast<UBlueprint>(UEditorAssetLibrary::LoadAsset(ToObjectPath(PackagePath)));
    if (!TestNotNull(TEXT("effect blueprint loads"), Blueprint)
        || !TestNotNull(TEXT("effect generated class exists"),
                Blueprint ? Blueprint->GeneratedClass.Get() : nullptr))
    {
        return true;
    }
    UGameplayEffect* EffectCDO = Cast<UGameplayEffect>(Blueprint->GeneratedClass->GetDefaultObject());
    if (!TestNotNull(TEXT("effect CDO casts to UGameplayEffect"), EffectCDO))
    {
        return true;
    }

    // THE load-bearing assertion: the live CDO must report the tag through
    // GetGrantedTags() — the same CachedGrantedTags (aggregated from GEComponents)
    // that UE's cooldown-GE validator reads. The deprecated-only write left this
    // empty; the component-model write makes it non-empty without any recompile.
    const FGameplayTag ExpectedTag =
        UGameplayTagsManager::Get().RequestGameplayTag(FName(*CooldownTag), false);
    TestTrue(TEXT("the cooldown tag is registered for the assertion"), ExpectedTag.IsValid());
    TestFalse(TEXT("GetGrantedTags() is non-empty (GE actually grants tags)"),
        EffectCDO->GetGrantedTags().IsEmpty());
    TestTrue(TEXT("GetGrantedTags() contains the granted tag (cooldown validator would pass)"),
        EffectCDO->GetGrantedTags().HasTagExact(ExpectedTag));

    // The readback must match the live CDO: get_gas_info reads grantedTags from the
    // component model now, so it lists the same tag the validator sees.
    {
        TSharedPtr<FJsonObject> InfoPayload = MakeShared<FJsonObject>();
        InfoPayload->SetStringField(TEXT("assetPath"), ToObjectPath(PackagePath));
        FTestResponseCapture InfoCapture;
        TestTrue(TEXT("gas.get_gas_info handler found"),
            InvokeHandlerWithCapture(TEXT("gas.get_gas_info"), InfoPayload, InfoCapture));
        if (TestTrue(TEXT("gas.get_gas_info succeeded"), InfoCapture.bSuccess))
        {
            TestTrue(TEXT("get_gas_info grantedTags reflects the component model"),
                JsonStringArrayContains(InfoCapture.Result, TEXT("grantedTags"), CooldownTag));
        }
    }

    return true;
#endif
}

// ============================================================================
// gas.add_effect_execution_calculation accepts the BARE exec-calc asset path
// (no _C suffix) — the exact path gas.create_execution_calculation returns —
// and rejects a non-exec-calc class with INVALID_TYPE.
// Regression for E-gas-add-execution-calc-requires-c-suffix.
//
// The defect: the handler resolved calculationClass with a raw
// LoadClass<UGameplayEffectExecutionCalculation>(nullptr, *Path), which only
// resolves a generated-class path (…/DamageMitigationExec.DamageMitigationExec_C)
// and hard-rejected the bare asset path (…/DamageMitigationExec — exactly what
// create_execution_calculation emits) with CLASS_NOT_FOUND, forcing a
// trial-and-error _C retry. The fix routes the param through the canonical
// ResolveUClass (which falls back to the UBlueprint's GeneratedClass for a bare
// content path) plus an IsChildOf(UGameplayEffectExecutionCalculation) guard.
//
// Counterfactual: revert to the raw LoadClass and the BARE-path add below fails
// with CLASS_NOT_FOUND (bSuccess false), failing the success assertion; drop the
// IsChildOf guard and the GameplayEffect-class arg below resolves and is wired as
// a bogus "execution calc", so the INVALID_TYPE rejection assertion fails.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGasAddEffectExecutionCalculationResolvesBarePathTest,
    "PinWright.gas.add_effect_execution_calculation.ResolvesBarePath",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGasAddEffectExecutionCalculationResolvesBarePathTest::RunTest(const FString& Parameters)
{
#if !MCP_GAS_TEST_HAS_GAS
    PinWrightTestSkip::SkipAssertions(*this, TEXT("optional-plugin-not-shipped"),
        TEXT("GameplayAbilities plugin not available; skipping."));
    return true;
#else
    // 1. Create a real exec-calc Blueprint via the production handler. CreateGASAsset
    //    returns the bare package path (/Game/__PW_GatewayTests/EC_…), the same form
    //    gas.create_execution_calculation echoes back as its assetPath — NO _C suffix.
    const FString ExecCalcPath = CreateGASAsset(
        *this, TEXT("gas.create_execution_calculation"), TEXT("EC_BarePath"),
        [](FJsonObject&) {});
    if (ExecCalcPath.IsEmpty())
    {
        return true;
    }
    ON_SCOPE_EXIT { CleanupTestAsset(ExecCalcPath); };

    // The exec-calc generated class — what a correct resolution must produce.
    UBlueprint* ExecCalcBlueprint =
        Cast<UBlueprint>(UEditorAssetLibrary::LoadAsset(ToObjectPath(ExecCalcPath)));
    if (!TestNotNull(TEXT("exec-calc blueprint loads"), ExecCalcBlueprint)
        || !TestNotNull(TEXT("exec-calc generated class exists"),
                ExecCalcBlueprint ? ExecCalcBlueprint->GeneratedClass.Get() : nullptr))
    {
        return true;
    }
    UClass* ExecCalcGenClass = ExecCalcBlueprint->GeneratedClass.Get();

    // 2. Create a GameplayEffect to carry the execution.
    const FString EffectPath = CreateEffect(*this, TEXT("GE_ExecBarePath"), TEXT("instant"));
    if (EffectPath.IsEmpty())
    {
        return true;
    }
    ON_SCOPE_EXIT { CleanupTestAsset(EffectPath); };
    const FString EffectObjectPath = ToObjectPath(EffectPath);

    // 3. THE load-bearing call: add the execution using the BARE exec-calc path
    //    (no _C). Before the fix this hard-rejected with CLASS_NOT_FOUND.
    {
        TSharedPtr<FJsonObject> ExecPayload = MakeShared<FJsonObject>();
        ExecPayload->SetStringField(TEXT("blueprintPath"), EffectObjectPath);
        ExecPayload->SetStringField(TEXT("calculationClass"), ExecCalcPath); // bare, no _C
        FTestResponseCapture ExecCapture;
        TestTrue(TEXT("gas.add_effect_execution_calculation handler found"),
            InvokeHandlerWithCapture(TEXT("gas.add_effect_execution_calculation"), ExecPayload, ExecCapture));
        if (!TestTrue(TEXT("add_effect_execution_calculation accepts the bare asset path (no _C)"),
                ExecCapture.bSuccess))
        {
            AddError(FString::Printf(TEXT("add_effect_execution_calculation failed on bare path: %s %s"),
                *ExecCapture.ErrorCode, *ExecCapture.Message));
            return true;
        }
    }

    // 4. The bare path must have resolved to the exec-calc generated class on the CDO.
    UBlueprint* EffectBlueprint =
        Cast<UBlueprint>(UEditorAssetLibrary::LoadAsset(EffectObjectPath));
    if (!TestNotNull(TEXT("effect blueprint loads"), EffectBlueprint)
        || !TestNotNull(TEXT("effect generated class exists"),
                EffectBlueprint ? EffectBlueprint->GeneratedClass.Get() : nullptr))
    {
        return true;
    }
    UGameplayEffect* EffectCDO =
        Cast<UGameplayEffect>(EffectBlueprint->GeneratedClass->GetDefaultObject());
    if (!TestNotNull(TEXT("effect CDO casts to UGameplayEffect"), EffectCDO))
    {
        return true;
    }
    if (TestEqual(TEXT("effect has exactly one execution"), EffectCDO->Executions.Num(), 1))
    {
        TestTrue(TEXT("execution CalculationClass resolved to the exec-calc generated class"),
            EffectCDO->Executions[0].CalculationClass.Get() == ExecCalcGenClass);
    }

    // 5. The IsChildOf guard: a class that resolves but is NOT a
    //    UGameplayEffectExecutionCalculation (the GameplayEffect's own bare path)
    //    must be rejected with INVALID_TYPE — not silently wired, not CLASS_NOT_FOUND.
    {
        TSharedPtr<FJsonObject> BadPayload = MakeShared<FJsonObject>();
        BadPayload->SetStringField(TEXT("blueprintPath"), EffectObjectPath);
        BadPayload->SetStringField(TEXT("calculationClass"), EffectPath); // a GE, not an exec calc
        FTestResponseCapture BadCapture;
        TestTrue(TEXT("gas.add_effect_execution_calculation handler found (wrong type)"),
            InvokeHandlerWithCapture(TEXT("gas.add_effect_execution_calculation"), BadPayload, BadCapture));
        TestFalse(TEXT("add_effect_execution_calculation rejects a non-exec-calc class"),
            BadCapture.bSuccess);
        TestEqual(TEXT("error code is INVALID_TYPE"),
            BadCapture.ErrorCode, FString(TEXT("INVALID_TYPE")));
    }

    return true;
#endif
}

// ============================================================================
// gas.get_gas_info — GameplayEffectExecutionCalculation branch reads back the
// captures written by gas.set_execution_capture
// Regression for E-gas-info-exec-calc-no-captures-readback.
//
// The defect: get_gas_info's blueprint else-if chain had no
// UGameplayEffectExecutionCalculation branch, so an exec-calc blueprint fell
// through to CollectAbilitySystemOwnerInfo, found no ASC, and returned bare
// metadata (no gasType, no captures). The named final step of an exec-calc
// authoring task — "read back the captures on the exec calc" — was unsatisfiable
// via the tool; the only readback was an out-of-band property.get on
// Default__<Exec>_C.RelevantAttributesToCapture. The fix adds the exec-calc
// branch: gasType:"GameplayEffectExecutionCalculation" + a captures array
// (attribute/source/snapshot) read from RelevantAttributesToCapture via the same
// reflection path gas.set_execution_capture writes through.
//
// Counterfactual: revert the exec-calc branch and get_gas_info on the exec calc
// returns no gasType and no captures, failing every assertion below.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGasGetGasInfoExecCalcEmitsCapturesTest,
    "PinWright.gas.get_gas_info.ExecCalcEmitsCaptures",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGasGetGasInfoExecCalcEmitsCapturesTest::RunTest(const FString& Parameters)
{
#if !MCP_GAS_TEST_HAS_GAS
    PinWrightTestSkip::SkipAssertions(*this, TEXT("optional-plugin-not-shipped"),
        TEXT("GameplayAbilities plugin not available; skipping."));
    return true;
#else
    // 1. An AttributeSet with two compiled attributes (AttackPower, Armor) — the
    //    capture targets. gas.add_attribute compiles each onto the generated class.
    const FString AttrSetPath = CreateAttributeSet(*this, TEXT("AS_ExecCaptures"));
    if (AttrSetPath.IsEmpty())
    {
        return true;
    }
    ON_SCOPE_EXIT { CleanupTestAsset(AttrSetPath); };
    const FString AttrSetObjectPath = ToObjectPath(AttrSetPath);
    const FString AttrSetName = FPackageName::GetLongPackageAssetName(AttrSetPath);

    const auto AddAttribute = [&](const TCHAR* AttributeName) -> bool
    {
        TSharedPtr<FJsonObject> AddPayload = MakeShared<FJsonObject>();
        AddPayload->SetStringField(TEXT("blueprintPath"), AttrSetObjectPath);
        AddPayload->SetStringField(TEXT("attributeName"), AttributeName);
        FTestResponseCapture AddCapture;
        TestTrue(TEXT("gas.add_attribute handler found"),
            InvokeHandlerWithCapture(TEXT("gas.add_attribute"), AddPayload, AddCapture));
        if (!TestTrue(FString::Printf(TEXT("gas.add_attribute %s succeeded"), AttributeName),
                AddCapture.bSuccess))
        {
            AddError(FString::Printf(TEXT("add_attribute %s failed: %s %s"),
                AttributeName, *AddCapture.ErrorCode, *AddCapture.Message));
            return false;
        }
        return true;
    };
    if (!AddAttribute(TEXT("AttackPower")) || !AddAttribute(TEXT("Armor")))
    {
        return true;
    }

    // The attribute spec set_execution_capture resolves: '<generated class>.AttrName'.
    const FString AttackPowerSpec =
        FString::Printf(TEXT("%s.%s_C.AttackPower"), *AttrSetPath, *AttrSetName);
    const FString ArmorSpec =
        FString::Printf(TEXT("%s.%s_C.Armor"), *AttrSetPath, *AttrSetName);

    // 2. A real exec-calc blueprint via the production handler (parent is
    //    UGameplayEffectExecutionCalculation, which the get_gas_info branch keys on).
    const FString ExecCalcPath = CreateGASAsset(
        *this, TEXT("gas.create_execution_calculation"), TEXT("EC_Captures"),
        [](FJsonObject&) {});
    if (ExecCalcPath.IsEmpty())
    {
        return true;
    }
    ON_SCOPE_EXIT { CleanupTestAsset(ExecCalcPath); };
    const FString ExecCalcObjectPath = ToObjectPath(ExecCalcPath);

    // 3. Populate two captures: AttackPower from Source snapshot=true, Armor from
    //    Target snapshot=false — the exact shape the ticket's repro describes.
    {
        const auto MakeCapture = [](const FString& AttrSpec, const TCHAR* Source, bool bSnapshot)
        {
            TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
            Entry->SetStringField(TEXT("attribute"), AttrSpec);
            Entry->SetStringField(TEXT("source"), Source);
            Entry->SetBoolField(TEXT("snapshot"), bSnapshot);
            return MakeShared<FJsonValueObject>(Entry);
        };
        TArray<TSharedPtr<FJsonValue>> Captures;
        Captures.Add(MakeCapture(AttackPowerSpec, TEXT("source"), true));
        Captures.Add(MakeCapture(ArmorSpec, TEXT("target"), false));

        TSharedPtr<FJsonObject> CapPayload = MakeShared<FJsonObject>();
        CapPayload->SetStringField(TEXT("executionClassPath"), ExecCalcObjectPath);
        CapPayload->SetArrayField(TEXT("captures"), Captures);
        FTestResponseCapture CapCapture;
        TestTrue(TEXT("gas.set_execution_capture handler found"),
            InvokeHandlerWithCapture(TEXT("gas.set_execution_capture"), CapPayload, CapCapture));
        if (!TestTrue(TEXT("gas.set_execution_capture succeeded"), CapCapture.bSuccess))
        {
            AddError(FString::Printf(TEXT("set_execution_capture failed: %s %s"),
                *CapCapture.ErrorCode, *CapCapture.Message));
            return true;
        }
    }

    // 4. The load-bearing call: get_gas_info on the exec calc must now stamp the
    //    exec-calc gasType and read the captures back (no out-of-band property.get).
    TSharedPtr<FJsonObject> InfoPayload = MakeShared<FJsonObject>();
    InfoPayload->SetStringField(TEXT("assetPath"), ExecCalcObjectPath);
    FTestResponseCapture InfoCapture;
    TestTrue(TEXT("gas.get_gas_info handler found"),
        InvokeHandlerWithCapture(TEXT("gas.get_gas_info"), InfoPayload, InfoCapture));
    if (!TestTrue(TEXT("gas.get_gas_info succeeded"), InfoCapture.bSuccess))
    {
        AddError(FString::Printf(TEXT("get_gas_info failed: %s %s"),
            *InfoCapture.ErrorCode, *InfoCapture.Message));
        return true;
    }

    FString GasType;
    InfoCapture.Result->TryGetStringField(TEXT("gasType"), GasType);
    if (!TestEqual(TEXT("gasType is GameplayEffectExecutionCalculation"),
            GasType, FString(TEXT("GameplayEffectExecutionCalculation"))))
    {
        return true;
    }

    const TArray<TSharedPtr<FJsonValue>>* CapturesOut = nullptr;
    if (!TestTrue(TEXT("captures array is present"),
            InfoCapture.Result->TryGetArrayField(TEXT("captures"), CapturesOut))
        || !TestEqual(TEXT("captures array has two entries"), CapturesOut->Num(), 2))
    {
        return true;
    }

    // Find a capture entry by attribute name (robust to ordering) and assert its
    // source + snapshot round-trip what set_execution_capture wrote.
    const auto AssertCapture =
        [&](const TCHAR* AttributeName, const TCHAR* ExpectedSource, bool bExpectedSnapshot)
    {
        for (const TSharedPtr<FJsonValue>& Value : *CapturesOut)
        {
            const TSharedPtr<FJsonObject> Entry = Value.IsValid() ? Value->AsObject() : nullptr;
            if (!Entry.IsValid())
            {
                continue;
            }
            FString Attr;
            Entry->TryGetStringField(TEXT("attribute"), Attr);
            if (Attr != AttributeName)
            {
                continue;
            }
            FString Source;
            Entry->TryGetStringField(TEXT("source"), Source);
            TestEqual(FString::Printf(TEXT("%s capture source"), AttributeName),
                Source, FString(ExpectedSource));
            bool bSnapshot = !bExpectedSnapshot;
            Entry->TryGetBoolField(TEXT("snapshot"), bSnapshot);
            TestEqual(FString::Printf(TEXT("%s capture snapshot"), AttributeName),
                bSnapshot, bExpectedSnapshot);
            return;
        }
        AddError(FString::Printf(TEXT("captures array is missing the %s entry"), AttributeName));
    };
    AssertCapture(TEXT("AttackPower"), TEXT("source"), true);
    AssertCapture(TEXT("Armor"), TEXT("target"), false);

    return true;
#endif
}

// ============================================================================
// gas.set_ability_input reports PROPERTY_NOT_FOUND (not PROPERTY_NOT_INT) when the
// target property is ABSENT, while keeping PROPERTY_NOT_INT for a present-but-wrong
// -type property — the two causes must carry DISTINCT machine-readable codes
// (E-set-ability-input-not-found-mislabeled).
//
// Repro shape: gas.create_gameplay_ability parents to vanilla UGameplayAbility,
// which has no AbilityInputID member, so the default cdo path cannot find the
// property. Before the fix the absent (!Prop) branch reused PROPERTY_NOT_INT (a
// type-mismatch code), colliding with the genuine not-int/byte branch and telling
// a caller who dispatches on the code to "fix a type" when it must ADD the property.
//
// Counterfactual: revert GASHandler.cpp's !Prop branch to PROPERTY_NOT_INT and the
// absent-property assertion below fails (Capture.ErrorCode would read
// PROPERTY_NOT_INT). The wrong-type half (an existing bool member,
// bReplicateInputDirectly) guards against an over-broad "fix" that relabels BOTH
// branches and loses the type signal.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGasSetAbilityInputMissingPropertyDistinctCodeTest,
    "PinWright.gas.set_ability_input.MissingPropertyDistinctFromWrongType",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGasSetAbilityInputMissingPropertyDistinctCodeTest::RunTest(const FString& Parameters)
{
#if !MCP_GAS_TEST_HAS_GAS
    PinWrightTestSkip::SkipAssertions(*this, TEXT("optional-plugin-not-shipped"),
        TEXT("GameplayAbilities plugin not available; skipping."));
    return true;
#else
    // Build the exact repro fixture in-code: a vanilla-parented GameplayAbility via
    // the plugin's own gas.create_gameplay_ability handler (no AbilityInputID member).
    const FString PackagePath = CreateAbility(*this, TEXT("GA_InputCodeSplit"));
    if (PackagePath.IsEmpty())
    {
        // CreateAbility already recorded an error on failure; this is a real
        // failure (missing required fixture), not a skip.
        return true;
    }
    ON_SCOPE_EXIT
    {
        CleanupTestAsset(PackagePath);
    };
    const FString AbilityObjectPath = ToObjectPath(PackagePath);

    // 1. Absent property (default propertyName "AbilityInputID") must report
    //    PROPERTY_NOT_FOUND, NOT the type-mismatch code PROPERTY_NOT_INT.
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("blueprintPath"), AbilityObjectPath);
        Payload->SetNumberField(TEXT("inputIDValue"), 1);

        FTestResponseCapture Capture;
        TestTrue(TEXT("gas.set_ability_input handler found (absent property)"),
            InvokeHandlerWithCapture(TEXT("gas.set_ability_input"), Payload, Capture));
        TestFalse(TEXT("set_ability_input rejects an absent property"), Capture.bSuccess);
        TestEqual(TEXT("absent property reports PROPERTY_NOT_FOUND, not PROPERTY_NOT_INT"),
            Capture.ErrorCode, FString(TEXT("PROPERTY_NOT_FOUND")));
    }

    // 2. Present-but-wrong-type property (bReplicateInputDirectly is a real bool
    //    UPROPERTY inherited from UGameplayAbility) must STILL report PROPERTY_NOT_INT,
    //    proving the two causes now carry distinct codes.
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("blueprintPath"), AbilityObjectPath);
        Payload->SetNumberField(TEXT("inputIDValue"), 1);
        Payload->SetStringField(TEXT("propertyName"), TEXT("bReplicateInputDirectly"));

        FTestResponseCapture Capture;
        TestTrue(TEXT("gas.set_ability_input handler found (wrong-type property)"),
            InvokeHandlerWithCapture(TEXT("gas.set_ability_input"), Payload, Capture));
        TestFalse(TEXT("set_ability_input rejects a non-int/byte property"), Capture.bSuccess);
        TestEqual(TEXT("wrong-type property still reports PROPERTY_NOT_INT"),
            Capture.ErrorCode, FString(TEXT("PROPERTY_NOT_INT")));
    }

    return true;
#endif
}
