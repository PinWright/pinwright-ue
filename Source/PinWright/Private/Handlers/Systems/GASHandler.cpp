// Copyright (c) 2026 Alexander Penkin. MIT License.

// GASHandler.cpp - Migrated from PinWright_GASHandlers.cpp
// Gameplay Ability System (GAS) handlers
// Implements 30 actions for abilities, effects, attributes, and gameplay cues.
//
// Actions:
// Components & Attributes: create_attribute_set, add_attribute,
//      set_attribute_base_value
// Abilities: create_gameplay_ability, set_ability_tags, set_ability_costs, set_ability_cooldown,
//      set_activation_policy, set_instancing_policy
// Effects: create_gameplay_effect, set_effect_duration, add_effect_modifier, set_modifier_magnitude,
//      add_effect_execution_calculation, add_effect_cue, set_effect_stacking, set_effect_tags
// Cues: create_gameplay_cue_notify
// Tags/Utility: add_tag_to_asset, get_gas_info
// Ability Sets: create_ability_set
// Execution Calculations: create_execution_calculation

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/Blueprint/BlueprintHandlerUtils.h"
#include "PinWrightHelpers.h"
#include "PinWrightSubsystem.h"
#include "Utils/PathUtils.h"
#include "Utils/ClassUtils.h"
#include "Utils/GuardedLoad.h"
#include "Utils/JsonBuilders.h"
#include "Compat/EngineVersionCompat.h"

DEFINE_LOG_CATEGORY_STATIC(LogMcpGASHandlers, Log, All);

#include "Engine/DataAsset.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "Factories/BlueprintFactory.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "UObject/SavePackage.h"
#include "Misc/PackageName.h"
#include "HAL/FileManager.h"
#include "GameplayTagsManager.h"
#include "GameplayTagContainer.h"
#include "EditorAssetLibrary.h"
#include "EdGraphSchema_K2.h"

// GAS module check
#if __has_include("AbilitySystemComponent.h")
#define MCP_HAS_GAS 1
#include "AbilitySystemComponent.h"
#include "AttributeSet.h"
#include "GameplayEffect.h"
#include "GameplayAbilitySpec.h"
#include "Abilities/GameplayAbility.h"
#include "GameplayCueNotify_Static.h"
#include "GameplayCueNotify_Actor.h"
#include "GameplayEffectExecutionCalculation.h"
// UE 5.3+ component-based granted-tags model. A GE grants tags through a
// UTargetTagsGameplayEffectComponent in GEComponents; the engine's cooldown-GE
// validator reads GetGrantedTags() (CachedGrantedTags, rebuilt from components),
// NOT the deprecated InheritableOwnedTagsContainer.
#if __has_include("GameplayEffectComponents/TargetTagsGameplayEffectComponent.h")
#define MCP_HAS_GE_TARGET_TAGS_COMPONENT 1
#include "GameplayEffectComponents/TargetTagsGameplayEffectComponent.h"
#else
#define MCP_HAS_GE_TARGET_TAGS_COMPONENT 0
#endif
#else
#define MCP_HAS_GAS 0
#define MCP_HAS_GE_TARGET_TAGS_COMPONENT 0
#endif

// ============================================================================
// Static helpers (only available when GAS headers are present)
// ============================================================================

#if MCP_HAS_GAS

static FGameplayTag GetOrRequestTag(const FString& TagString)
{
    return FGameplayTag::RequestGameplayTag(FName(*TagString), false);
}

// Builds the SendError data object carrying the list of unregistered tags that a
// tag-write verb refused to drop silently. Shared by the GAS tag-write handlers
// (set_effect_tags / set_ability_tags) so they report dropped tags identically to
// ai.configure_slot_behavior's droppedTags convention.
static TSharedPtr<FJsonObject> MakeDroppedTagsErrData(const TArray<FString>& DroppedTags)
{
    TSharedPtr<FJsonObject> ErrData = MakeShared<FJsonObject>();
    ErrData->SetArrayField(TEXT("droppedTags"), EmitStringArray(DroppedTags));
    return ErrData;
}

// Validate-before-mutate tag resolution shared by the GAS tag-write handlers
// (set_ability_tags / set_effect_tags). For each requested tag string: skip empties,
// resolve against the registry, and partition the result — a valid FGameplayTag goes
// into Out (and, when AddedNames is non-null, its source string is recorded there);
// an unregistered tag is collected into Dropped rather than silently skipped. Callers
// reject the whole call when Dropped is non-empty.
static void ResolveTagsInto(const TArray<TSharedPtr<FJsonValue>>& In,
                            TArray<FGameplayTag>& Out,
                            TArray<FString>& Dropped,
                            TArray<FString>* AddedNames)
{
    for (const auto& TagValue : In)
    {
        FString TagStr = TagValue->AsString();
        if (TagStr.IsEmpty())
        {
            continue;
        }
        FGameplayTag Tag = GetOrRequestTag(TagStr);
        if (Tag.IsValid())
        {
            Out.Add(Tag);
            if (AddedNames)
            {
                AddedNames->Add(TagStr);
            }
        }
        else
        {
            Dropped.Add(TagStr);
        }
    }
}

// Grant tags on a GameplayEffect through the UE 5.3+ component model.
//
// In UE 5.3+ the tags a GE grants live in a UTargetTagsGameplayEffectComponent
// inside GEComponents; the engine's cooldown-GE validator reads them back via
// GetGrantedTags() (CachedGrantedTags, aggregated from GEComponents) and NOT from
// the deprecated InheritableOwnedTagsContainer. Writing only the deprecated field
// (as the handlers used to) leaves GetGrantedTags() empty for a freshly created GE,
// because the deprecated container is migrated into a component only by the engine's
// one-time pre-Modular53 ConvertTargetTagsComponent() upgrade, which a GE authored
// fresh in 5.7 never triggers.
//
// We mirror the engine's own bridge (GameplayEffect.cpp ConvertTargetTagsComponent):
// find-or-add the component, seed an FInheritedTagContainer with the requested tags
// (plus any already configured on the component, so repeated calls are additive),
// and SetAndApplyTargetTagChanges — which applies straight to CachedGrantedTags so
// GetGrantedTags() is correct immediately, even on the CDO with no recompile. On the
// component path we deliberately do NOT touch InheritableOwnedTagsContainer: the engine
// owns that field and reverse-syncs it FROM the component (ConvertTargetTagsComponent
// assigns InheritableOwnedTagsContainer = component->GetConfiguredTargetTagChanges() on
// the next recompile), so any additive write here is write-only dead state that the
// engine clobbers — and the component is the single source of truth GetGrantedTags()
// (and our CollectGrantedTags readback) reads.
//
// On engines that predate the component header (≤5.2), MCP_HAS_GE_TARGET_TAGS_COMPONENT
// is 0 and the deprecated container IS the storage, so we write it directly there.
static void GrantTagsOnEffect(UGameplayEffect* EffectCDO, const TArray<FGameplayTag>& Tags)
{
    if (!EffectCDO || Tags.Num() == 0)
    {
        return;
    }

#if MCP_HAS_GE_TARGET_TAGS_COMPONENT
    UTargetTagsGameplayEffectComponent& TargetTagsComponent =
        EffectCDO->FindOrAddComponent<UTargetTagsGameplayEffectComponent>();

    // Start from the component's currently configured tags so multiple
    // set_effect_tags calls accumulate rather than clobber.
    FInheritedTagContainer Configured = TargetTagsComponent.GetConfiguredTargetTagChanges();
    for (const FGameplayTag& Tag : Tags)
    {
#if UE_VERSION_OLDER_THAN(5, 4, 0)
        // UE 5.3's FInheritedTagContainer::AddTag only writes CombinedTags, NOT Added.
        // SetAndApplyTargetTagChanges then calls UpdateInheritedTagProperties, which
        // resets CombinedTags and rebuilds it solely from Added — wiping the tag and
        // leaving GetGrantedTags() empty. Mirror the engine's own 5.4+ AddTag (which
        // populates Added/Removed/CombinedTags) so the tag survives the recompute.
        Configured.Removed.RemoveTag(Tag);
        Configured.Added.AddTag(Tag);
        Configured.CombinedTags.AddTag(Tag);
#else
        Configured.AddTag(Tag);
#endif
    }
    TargetTagsComponent.SetAndApplyTargetTagChanges(Configured);
#else
    // No component model on this engine: the deprecated container is the storage.
    PRAGMA_DISABLE_DEPRECATION_WARNINGS
    for (const FGameplayTag& Tag : Tags)
    {
        EffectCDO->InheritableOwnedTagsContainer.AddTag(Tag);
    }
    PRAGMA_ENABLE_DEPRECATION_WARNINGS
#endif
}

// Read back the tags a GameplayEffect grants, matching what the engine's
// cooldown-GE validator sees. Prefers GetGrantedTags() (CachedGrantedTags, rebuilt
// from GEComponents) so the readback agrees with the live CDO and the validator;
// falls back to the deprecated container only on engines without the component model.
static TArray<FString> CollectGrantedTags(const UGameplayEffect* EffectCDO)
{
    TArray<FString> Names;
    if (!EffectCDO)
    {
        return Names;
    }

#if MCP_HAS_GE_TARGET_TAGS_COMPONENT
    for (const FGameplayTag& Tag : EffectCDO->GetGrantedTags())
    {
        Names.Add(Tag.ToString());
    }
#else
    PRAGMA_DISABLE_DEPRECATION_WARNINGS
    for (const FGameplayTag& Tag : EffectCDO->InheritableOwnedTagsContainer.CombinedTags)
    {
        Names.Add(Tag.ToString());
    }
    PRAGMA_ENABLE_DEPRECATION_WARNINGS
#endif
    return Names;
}

// Read back each modifier on a GameplayEffect CDO as a JSON object so the get_gas_info
// readback can confirm an add_effect_modifier / set_modifier_magnitude / set_modifier_attribute
// run, not just the modifier count. Per entry:
//   operation     — the engine's op name string (EGameplayModOpToString, e.g. "AddBase")
//   magnitude     — the static scalar at level 1 where derivable (ScalableFloat magnitudes);
//                   GetStaticMagnitudeIfPossible returns false for AttributeBased/Custom/
//                   SetByCaller, in which case the field is omitted (magnitudeType still names it)
//   magnitudeType — the EGameplayEffectMagnitudeCalculation as a readable string
//   attribute     — the bound FGameplayAttribute name (gas.add_effect_modifier /
//                   gas.set_modifier_attribute), empty when the modifier is unbound
static TArray<TSharedPtr<FJsonValue>> CollectEffectModifiers(const UGameplayEffect* EffectCDO)
{
    TArray<TSharedPtr<FJsonValue>> Out;
    if (!EffectCDO)
    {
        return Out;
    }

    for (const FGameplayModifierInfo& Mod : EffectCDO->Modifiers)
    {
        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("operation"),
            EGameplayModOpToString(static_cast<int32>(Mod.ModifierOp.GetValue())));

        float StaticMag = 0.0f;
        if (Mod.ModifierMagnitude.GetStaticMagnitudeIfPossible(1.0f, StaticMag))
        {
            Entry->SetNumberField(TEXT("magnitude"), StaticMag);
        }
        Entry->SetStringField(TEXT("magnitudeType"),
            JsonBuilders::EnumValueToString(Mod.ModifierMagnitude.GetMagnitudeCalculationType()));

        // Empty string = unbound modifier (the legacy add_effect_modifier behavior
        // when no attribute is supplied).
        Entry->SetStringField(TEXT("attribute"),
            Mod.Attribute.IsValid() ? Mod.Attribute.GetName() : FString());

        Out.Add(MakeShared<FJsonValueObject>(Entry));
    }
    return Out;
}

// Locate a named float subproperty (BaseValue / CurrentValue) of FGameplayAttributeData.
// Centralizes the magic field-name strings and the StaticStruct + FindPropertyByName +
// CastField<FNumericProperty> dance so the attribute readback (CollectAttributeSetAttributes)
// and the attribute writer (gas.set_attribute_base_value) share one definition of where a
// field lives — a typo or a struct-layout change is fixed in a single place.
static FNumericProperty* GetAttributeDataFieldProp(const TCHAR* FieldName)
{
    UScriptStruct* AttrDataStruct = FGameplayAttributeData::StaticStruct();
    return AttrDataStruct
        ? CastField<FNumericProperty>(AttrDataStruct->FindPropertyByName(FieldName))
        : nullptr;
}

// Read a float subproperty of an FGameplayAttributeData instance (the value pointed at by
// AttrDataPtr). Returns false when the field can't be resolved, leaving Out untouched.
static bool ReadAttributeDataField(const void* AttrDataPtr, const TCHAR* FieldName, double& Out)
{
    if (!AttrDataPtr)
    {
        return false;
    }
    FNumericProperty* FieldProp = GetAttributeDataFieldProp(FieldName);
    if (!FieldProp)
    {
        return false;
    }
    Out = FieldProp->GetFloatingPointPropertyValue(FieldProp->ContainerPtrToValuePtr<void>(AttrDataPtr));
    return true;
}

// Write a float subproperty of an FGameplayAttributeData instance. Returns false when the
// field can't be resolved (leaving the instance unchanged).
static bool WriteAttributeDataField(void* AttrDataPtr, const TCHAR* FieldName, double Value)
{
    if (!AttrDataPtr)
    {
        return false;
    }
    FNumericProperty* FieldProp = GetAttributeDataFieldProp(FieldName);
    if (!FieldProp)
    {
        return false;
    }
    FieldProp->SetFloatingPointPropertyValue(FieldProp->ContainerPtrToValuePtr<void>(AttrDataPtr), Value);
    return true;
}

// Read back each attribute declared on an AttributeSet's generated class as a JSON
// object so the get_gas_info AttributeSet branch can confirm a gas.add_attribute /
// gas.set_attribute_base_value run. Each attribute is an FGameplayAttributeData
// FStructProperty on the generated class; its default lives in the BaseValue numeric
// subproperty on the CDO (the same field gas.set_attribute_base_value writes). Per entry:
//   name      — the attribute (FProperty) name
//   baseValue — the FGameplayAttributeData::BaseValue read off the CDO
static TArray<TSharedPtr<FJsonValue>> CollectAttributeSetAttributes(const UClass* GeneratedClass)
{
    TArray<TSharedPtr<FJsonValue>> Out;
    if (!GeneratedClass)
    {
        return Out;
    }

    const UObject* AttrSetCDO = GeneratedClass->GetDefaultObject();
    UScriptStruct* AttrDataStruct = FGameplayAttributeData::StaticStruct();

    for (TFieldIterator<FStructProperty> It(GeneratedClass); It; ++It)
    {
        FStructProperty* StructProp = *It;
        if (!StructProp || StructProp->Struct != AttrDataStruct)
        {
            continue;
        }

        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("name"), StructProp->GetName());

        double BaseValue = 0.0;
        if (ReadAttributeDataField(StructProp->ContainerPtrToValuePtr<void>(AttrSetCDO),
                TEXT("BaseValue"), BaseValue))
        {
            Entry->SetNumberField(TEXT("baseValue"), BaseValue);
        }

        Out.Add(MakeShared<FJsonValueObject>(Entry));
    }
    return Out;
}

// Read back each capture in RelevantAttributesToCapture off a
// GameplayEffectExecutionCalculation generated-class CDO so the get_gas_info
// exec-calc branch can confirm a gas.set_execution_capture run from this tool
// instead of an out-of-band property.get on Default__<Exec>_C. The field is
// protected on UGameplayEffectCalculation, so this reads it through the engine's
// public GetAttributeCaptureDefinitions() accessor (the writer can't use it — there
// is no public mutator — and keeps its own reflection path). Per entry:
//   attribute — the captured FGameplayAttribute name ("" when invalid)
//   source    — "source" or "target" (from AttributeSource)
//   snapshot  — bSnapshot
static TArray<TSharedPtr<FJsonValue>> CollectExecutionCaptures(const UClass* GeneratedClass)
{
    TArray<TSharedPtr<FJsonValue>> Out;
    if (!GeneratedClass)
    {
        return Out;
    }

    const UGameplayEffectCalculation* CalcCDO =
        Cast<UGameplayEffectCalculation>(GeneratedClass->GetDefaultObject());
    if (!CalcCDO)
    {
        return Out;
    }

    for (const FGameplayEffectAttributeCaptureDefinition& Capture : CalcCDO->GetAttributeCaptureDefinitions())
    {
        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("attribute"),
            Capture.AttributeToCapture.IsValid() ? Capture.AttributeToCapture.GetName() : FString());
        Entry->SetStringField(TEXT("source"),
            Capture.AttributeSource == EGameplayEffectAttributeCaptureSource::Target
                ? TEXT("target") : TEXT("source"));
        Entry->SetBoolField(TEXT("snapshot"), Capture.bSnapshot);

        Out.Add(MakeShared<FJsonValueObject>(Entry));
    }
    return Out;
}

template<typename T>
static bool SetAbilityPropertyValue(UGameplayAbility* Ability, const FName& PropertyName, const T& Value)
{
    if (!Ability) return false;

    FProperty* Prop = Ability->GetClass()->FindPropertyByName(PropertyName);
    if (!Prop) return false;

    void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(Ability);
    if (!ValuePtr) return false;

    *static_cast<T*>(ValuePtr) = Value;
    return true;
}

template<typename T>
static bool GetAbilityPropertyValue(const UGameplayAbility* Ability, const FName& PropertyName, T& OutValue)
{
    if (!Ability) return false;

    FProperty* Prop = Ability->GetClass()->FindPropertyByName(PropertyName);
    if (!Prop) return false;

    const void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(Ability);
    if (!ValuePtr) return false;

    OutValue = *static_cast<const T*>(ValuePtr);
    return true;
}

// Read back the AbilityTags a GameplayAbility carries, matching what
// gas.set_ability_tags writes. On 5.7+ AbilityTags is deprecated and surfaced via
// GetAssetTags() (the same container gas.set_ability_tags snapshots and reassigns);
// pre-5.7 the deprecated member is read directly. Mirrors CollectGrantedTags on the
// GameplayEffect branch so the readback agrees with the live CDO.
static TArray<FString> CollectAbilityTags(const UGameplayAbility* AbilityCDO)
{
    TArray<FString> Names;
    if (!AbilityCDO)
    {
        return Names;
    }

#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 7, 0)
    for (const FGameplayTag& Tag : AbilityCDO->GetAssetTags())
    {
        Names.Add(Tag.ToString());
    }
#else
    PRAGMA_DISABLE_DEPRECATION_WARNINGS
    for (const FGameplayTag& Tag : AbilityCDO->AbilityTags)
    {
        Names.Add(Tag.ToString());
    }
    PRAGMA_ENABLE_DEPRECATION_WARNINGS
#endif
    return Names;
}

// Resolve a named FGameplayTagContainer UPROPERTY on a GameplayAbility by reflection,
// returning a pointer into the instance's container or nullptr when the property is
// absent or is not a gameplay-tag container. Single type-guard shared by the symmetric
// AddTagToAbilityContainer writer and CollectAbilityContainerTags reader so the two
// can't drift on how a tag container is located/validated. (Not GetAbilityPropertyValue,
// which omits this StaticStruct guard and would type-confuse a same-named non-container.)
static FGameplayTagContainer* ResolveAbilityTagContainer(UGameplayAbility* Ability, const FName& PropertyName)
{
    if (!Ability)
    {
        return nullptr;
    }
    FProperty* Prop = Ability->GetClass()->FindPropertyByName(PropertyName);
    FStructProperty* StructProp = CastField<FStructProperty>(Prop);
    if (!StructProp || StructProp->Struct != FGameplayTagContainer::StaticStruct())
    {
        return nullptr;
    }
    void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(Ability);
    if (!ValuePtr)
    {
        return nullptr;
    }
    return static_cast<FGameplayTagContainer*>(ValuePtr);
}

// Read back a named FGameplayTagContainer UPROPERTY off a GameplayAbility CDO — the
// symmetric reader for AddTagToAbilityContainer's write (both go through
// ResolveAbilityTagContainer). Used by the get_gas_info readback to surface the
// activation-gating containers (ActivationBlockedTags / ActivationRequiredTags /
// ActivationOwnedTags) that gas.set_ability_tags authors, so "confirm the ability is
// blocked while stunned" is satisfiable without an asset.dump. Returns empty when the
// property is absent or is not a gameplay-tag container.
static TArray<FString> CollectAbilityContainerTags(const UGameplayAbility* AbilityCDO, const FName& PropertyName)
{
    TArray<FString> Names;
    // const_cast is read-only here: the resolved container is only iterated, never mutated.
    const FGameplayTagContainer* Container =
        ResolveAbilityTagContainer(const_cast<UGameplayAbility*>(AbilityCDO), PropertyName);
    if (!Container)
    {
        return Names;
    }
    for (const FGameplayTag& Tag : *Container)
    {
        Names.Add(Tag.ToString());
    }
    return Names;
}

// Read a TSubclassOf<UGameplayEffect> property off the ability CDO (the cooldown/
// cost GE class written by gas.set_ability_cooldown / gas.set_ability_costs) and
// return its object path, or an empty string when unset. Reads through the same
// symmetric getter that pairs with the SetAbilityPropertyValue write the setters
// use, so the inspect-after-mutate readback mirrors how the value was stored.
static FString ReadAbilityEffectClassPath(const UGameplayAbility* AbilityCDO, const FName& PropertyName)
{
    TSubclassOf<UGameplayEffect> EffectClass;
    if (GetAbilityPropertyValue(AbilityCDO, PropertyName, EffectClass) && EffectClass)
    {
        return EffectClass->GetPathName();
    }
    return FString();
}

static bool AddTagToAbilityContainer(UGameplayAbility* Ability, const FName& PropertyName, const FGameplayTag& Tag)
{
    if (!Tag.IsValid()) return false;

    FGameplayTagContainer* Container = ResolveAbilityTagContainer(Ability, PropertyName);
    if (!Container) return false;

    Container->AddTag(Tag);
    return true;
}

static FString ReplicationModeToString(EGameplayEffectReplicationMode Mode)
{
    switch (Mode)
    {
    case EGameplayEffectReplicationMode::Minimal: return TEXT("minimal");
    case EGameplayEffectReplicationMode::Mixed:   return TEXT("mixed");
    case EGameplayEffectReplicationMode::Full:    return TEXT("full");
    default: break;
    }
    return FString();
}

// Maps an ASC template's replication mode to the full/mixed/minimal vocabulary for
// the get_gas_info readback. ReplicationMode is a public (non-UPROPERTY) member set
// at runtime by SetReplicationMode, so read it directly rather than via reflection
// (no FProperty exists for it, and it is not authorable on the template/CDO).
static FString AscReplicationModeToString(const UAbilitySystemComponent* ASC)
{
    return ASC ? ReplicationModeToString(ASC->ReplicationMode) : FString();
}

// Single source of truth for "does this blueprint OWN an AbilitySystemComponent".
// Walks the SCS component templates (class-authored ASCs) first; only if none are
// found does it fall back to a native ASC on the generated-class CDO (a C++ parent
// that owns one). Invokes Fn once per discovered ASC with its display name and a
// bNative flag distinguishing the two sources. Used by the get_gas_info readback
// (CollectAbilitySystemOwnerInfo) to detect actor blueprints that own an ASC.
static void ForEachOwnedASC(UBlueprint* Blueprint, TFunctionRef<void(UAbilitySystemComponent*, FName, bool)> Fn)
{
    if (!Blueprint)
    {
        return;
    }

    bool bFoundScs = false;
    if (Blueprint->SimpleConstructionScript)
    {
        for (USCS_Node* Node : Blueprint->SimpleConstructionScript->GetAllNodes())
        {
            if (Node)
            {
                if (UAbilitySystemComponent* ASC = Cast<UAbilitySystemComponent>(Node->ComponentTemplate))
                {
                    bFoundScs = true;
                    Fn(ASC, Node->GetVariableName(), /*bNative=*/false);
                }
            }
        }
    }

    if (!bFoundScs && Blueprint->GeneratedClass)
    {
        if (AActor* CDO = Cast<AActor>(Blueprint->GeneratedClass->GetDefaultObject()))
        {
            if (UAbilitySystemComponent* ASC = CDO->FindComponentByClass<UAbilitySystemComponent>())
            {
                Fn(ASC, FName(*ASC->GetName()), /*bNative=*/true);
            }
        }
    }
}

// Resolves an FGameplayAttribute from the canonical "AttributeSetClassPath.AttrName"
// spec used across the GAS surface (the same form gas.set_execution_capture accepts):
// split on the last '.', LoadObject<UClass> the attribute set, FindPropertyByName the
// attribute property, then construct FGameplayAttribute(Prop). On failure, fills
// OutErrCode/OutErrMsg with a caller-quotable diagnostic and returns false so the
// handler can SendError without inventing its own messages. Shared by the modifier
// attribute-binding handlers (gas.add_effect_modifier / gas.set_modifier_attribute)
// so they bind attributes exactly like set_execution_capture does.
static bool ResolveGameplayAttributeFromSpec(const FString& AttrSpec,
    FGameplayAttribute& OutAttr, FString& OutErrCode, FString& OutErrMsg)
{
    int32 DotIdx = INDEX_NONE;
    if (!AttrSpec.FindLastChar(TEXT('.'), DotIdx))
    {
        OutErrCode = TEXT("INVALID_PARAMS");
        OutErrMsg = FString::Printf(TEXT("Attribute '%s' must be in 'AttributeSetClassPath.AttrName' form"), *AttrSpec);
        return false;
    }
    const FString AttrSetPath = AttrSpec.Left(DotIdx);
    const FString AttrName = AttrSpec.Mid(DotIdx + 1);

    // Guarded, and this is the only load in this file that needs it: AttrSpec arrives as
    // `captures[].attribute`, a value inside an ARRAY ELEMENT, which the dispatch-boundary type
    // gate cannot see - it reads top-level params only, and FParamSpec::NestedKeys is an untyped
    // allow-list of keys. Board B-nested-path-values-reach-createpackage-fatal.
    FString AttrSetRefusal;
    UClass* AttrSetClass =
        PinWrightGuardedLoad::LoadObjectChecked<UClass>(AttrSetPath, &AttrSetRefusal);
    if (!AttrSetClass)
    {
        if (!AttrSetRefusal.IsEmpty())
        {
            OutErrCode = TEXT("INVALID_ARGUMENT");
            OutErrMsg = AttrSetRefusal;
            return false;
        }
        OutErrCode = TEXT("ATTRIBUTE_SET_NOT_FOUND");
        OutErrMsg = FString::Printf(TEXT("Could not load attribute set class '%s'"), *AttrSetPath);
        return false;
    }
    FProperty* AttrProperty = AttrSetClass->FindPropertyByName(FName(*AttrName));
    if (!AttrProperty)
    {
        OutErrCode = TEXT("ATTRIBUTE_NOT_FOUND");
        OutErrMsg = FString::Printf(TEXT("Attribute set '%s' has no compiled property '%s'"), *AttrSetPath, *AttrName);
        return false;
    }

    OutAttr = FGameplayAttribute(AttrProperty);
    return true;
}

// Loads the GameplayEffect CDO from 'blueprintPath' and resolves 'modifierIndex'
// into OutEffectCDO/OutModifierIndex, the shared preamble the modifier-mutating
// handlers (gas.set_modifier_magnitude / gas.set_modifier_attribute) repeat:
// empty-path check (INVALID_ARGUMENT), LoadObject<UBlueprint> + GeneratedClass
// (NOT_FOUND), Cast to UGameplayEffect CDO (INVALID_TYPE), and a both-ends bounds
// check on modifierIndex (INVALID_INDEX). On failure it SendErrors via Ctx and
// returns false so the caller can early-out without re-typing the diagnostics.
static bool ResolveEffectAndModifier(FHandlerContext& Ctx,
    UBlueprint*& OutBlueprint, UGameplayEffect*& OutEffectCDO, int32& OutModifierIndex)
{
    const FString BlueprintPath = Ctx.GetString(TEXT("blueprintPath"));
    if (BlueprintPath.IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("Missing blueprintPath."));
        return false;
    }

    OutBlueprint = LoadObject<UBlueprint>(nullptr, *BlueprintPath);
    if (!OutBlueprint || !OutBlueprint->GeneratedClass)
    {
        Ctx.SendError(TEXT("NOT_FOUND"), FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintPath));
        return false;
    }

    OutEffectCDO = Cast<UGameplayEffect>(OutBlueprint->GeneratedClass->GetDefaultObject());
    if (!OutEffectCDO)
    {
        Ctx.SendError(TEXT("INVALID_TYPE"), TEXT("Not a GameplayEffect blueprint"));
        return false;
    }

    OutModifierIndex = Ctx.GetInt(TEXT("modifierIndex"), 0);
    if (OutModifierIndex < 0 || OutModifierIndex >= OutEffectCDO->Modifiers.Num())
    {
        Ctx.SendError(TEXT("INVALID_INDEX"), TEXT("Modifier index out of range"));
        return false;
    }

    return true;
}

// Resolves a UClass-subclass argument the shared validate-before-mutate way the
// GAS setters need: route Path through the canonical forgiving ResolveUClass so a
// plain blueprint-asset path (/Game/.../X) works as well as the generated-class
// path (/Game/.../X.X_C — the E-class-name-format-inconsistency contract), then
// reject (never silent-success) with NotFoundCode when nothing resolves and
// INVALID_TYPE when the resolved class is not a T. NotFoundSubject is the
// not-found message subject ("Cost GameplayEffect class" / "Calculation class");
// TypeNoun names the type in the invalid-type message ("GameplayEffect class" /
// "GameplayEffectExecutionCalculation class"). On failure it SendErrors via Ctx
// and returns false so the caller early-outs.
template <typename T>
static bool ResolveGasSubclassArg(FHandlerContext& Ctx, const FString& Path,
    const TCHAR* NotFoundCode, const FString& NotFoundSubject, const TCHAR* TypeNoun,
    TSubclassOf<T>& OutClass)
{
    UClass* ResolvedClass = ResolveUClass(Path);
    if (!ResolvedClass)
    {
        Ctx.SendError(NotFoundCode,
            FString::Printf(TEXT("%s not found: %s"), *NotFoundSubject, *Path));
        return false;
    }
    if (!ResolvedClass->IsChildOf(T::StaticClass()))
    {
        Ctx.SendError(TEXT("INVALID_TYPE"),
            FString::Printf(TEXT("Not a %s: %s"), TypeNoun, *Path));
        return false;
    }
    OutClass = ResolvedClass;
    return true;
}

// Thin UGameplayEffect-typed wrapper the gas.set_ability_costs /
// gas.set_ability_cooldown setters call: Label ("Cost"/"Cooldown") names the slot
// in the not-found message, NOT_FOUND on a miss
// (B-set-ability-cooldown-cost-class-path-silent-drop).
static bool ResolveGameplayEffectClassArg(FHandlerContext& Ctx, const FString& Path,
    const TCHAR* Label, TSubclassOf<UGameplayEffect>& OutClass)
{
    return ResolveGasSubclassArg<UGameplayEffect>(Ctx, Path, TEXT("NOT_FOUND"),
        FString::Printf(TEXT("%s GameplayEffect class"), Label),
        TEXT("GameplayEffect class"), OutClass);
}

// Detects an actor blueprint that OWNS an AbilitySystemComponent and, when found,
// stamps gasType:"AbilitySystemOwner" plus an abilitySystemComponents list
// (component name + replication mode) onto Result. Uses the shared ForEachOwnedASC
// detection so get_gas_info no longer reports an ASC-owning actor as having no GAS
// data. No-op when no ASC is present.
static void CollectAbilitySystemOwnerInfo(UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& Result)
{
    if (!Blueprint || !Result.IsValid())
    {
        return;
    }

    TArray<TSharedPtr<FJsonValue>> AscJson;
    ForEachOwnedASC(Blueprint, [&AscJson](UAbilitySystemComponent* ASC, FName Name, bool bNative)
    {
        TSharedPtr<FJsonObject> AscObj = MakeShared<FJsonObject>();
        AscObj->SetStringField(TEXT("name"), Name.ToString());
        AscObj->SetStringField(TEXT("source"), bNative ? TEXT("native") : TEXT("scs"));
        const FString RepMode = AscReplicationModeToString(ASC);
        if (!RepMode.IsEmpty())
        {
            AscObj->SetStringField(TEXT("replicationMode"), RepMode);
        }
        AscJson.Add(MakeShared<FJsonValueObject>(AscObj));
    });

    if (AscJson.Num() > 0)
    {
        Result->SetStringField(TEXT("gasType"), TEXT("AbilitySystemOwner"));
        Result->SetArrayField(TEXT("abilitySystemComponents"), AscJson);
    }
}

static UBlueprint* CreateGASBlueprint(const FString& Path, const FString& Name, UClass* ParentClass, FString& OutError)
{
    if (!ParentClass)
    {
        OutError = TEXT("Invalid parent class");
        return nullptr;
    }

    FString PackageName;
    FString PathError;
    FString SanitizedName = SanitizeAssetName(Name);
    if (!ValidateAssetCreationPath(Path, SanitizedName, PackageName, PathError))
    {
        OutError = PathError;
        return nullptr;
    }

    if (!IsValidAssetPath(PackageName))
    {
        OutError = FString::Printf(TEXT("Invalid asset path: %s"), *PackageName);
        return nullptr;
    }

    UPackage* Package = CreatePackage(*PackageName);
    if (!Package)
    {
        OutError = FString::Printf(TEXT("Failed to create package: %s"), *PackageName);
        return nullptr;
    }

    UBlueprintFactory* Factory = NewObject<UBlueprintFactory>();
    Factory->ParentClass = ParentClass;

    UBlueprint* Blueprint = Cast<UBlueprint>(
        Factory->FactoryCreateNew(UBlueprint::StaticClass(), Package, FName(*SanitizedName),
                                  RF_Public | RF_Standalone, nullptr, GWarn));

    if (!Blueprint)
    {
        OutError = TEXT("Failed to create blueprint");
        return nullptr;
    }

    FAssetRegistryModule::AssetCreated(Blueprint);
    Blueprint->MarkPackageDirty();
    return Blueprint;
}

#endif

// ============================================================================
// 13.1 COMPONENTS & ATTRIBUTES
// ============================================================================

// ---- gas.create_attribute_set ----
REGISTER_RPC_HANDLER("gas.create_attribute_set", "gas", "Create a new AttributeSet blueprint",
    RPC_PARAMS(
        RPC_PARAM_REQ("name", "string", "Name for the attribute set"),
        RPC_PARAM_DEF("path", "path", "Content path for the asset", "/Game")
    ))
{
#if !MCP_HAS_GAS
    Ctx.SendError(TEXT("GAS_NOT_AVAILABLE"), TEXT("GameplayAbilities plugin not enabled."));
    return true;
#else
    FString Name = Ctx.GetString(TEXT("name"));
    FString Path = Ctx.GetString(TEXT("path"), TEXT("/Game"));

    if (Name.IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("Missing name."));
        return true;
    }

    FString Error;
    UBlueprint* Blueprint = CreateGASBlueprint(Path, Name, UAttributeSet::StaticClass(), Error);
    if (!Blueprint)
    {
        Ctx.SendError(TEXT("CREATION_FAILED"), Error);
        return true;
    }

    McpSafeAssetSave(Blueprint);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("name"), Name);
    Result->SetStringField(TEXT("parentClass"), TEXT("AttributeSet"));
    AddAssetVerification(Result, Blueprint);
    Ctx.SendSuccess(Result);
    return true;
#endif
}

// ---- gas.add_attribute ----
REGISTER_RPC_HANDLER("gas.add_attribute", "gas", "Add a gameplay attribute to an AttributeSet blueprint",
    RPC_PARAMS(
        RPC_PARAM_REQ("blueprintPath", "path", "Path to the AttributeSet blueprint"),
        RPC_PARAM_REQ("attributeName", "string", "Name of the attribute to add"),
        RPC_PARAM_DEF("defaultValue", "number", "Default value for the attribute", "0")
    ))
{
#if !MCP_HAS_GAS
    Ctx.SendError(TEXT("GAS_NOT_AVAILABLE"), TEXT("GameplayAbilities plugin not enabled."));
    return true;
#else
    FString BlueprintPath = Ctx.GetString(TEXT("blueprintPath"));
    if (BlueprintPath.IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("Missing blueprintPath."));
        return true;
    }

    FString AttributeName = Ctx.GetString(TEXT("attributeName"));
    if (AttributeName.IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("Missing attributeName."));
        return true;
    }

    UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *BlueprintPath);
    if (!Blueprint)
    {
        Ctx.SendError(TEXT("NOT_FOUND"), FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintPath));
        return true;
    }

    float DefaultValue = static_cast<float>(Ctx.GetNumber(TEXT("defaultValue"), 0.0));

    FEdGraphPinType PinType;
    PinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
    PinType.PinSubCategoryObject = FGameplayAttributeData::StaticStruct();

    bool bSuccess = FBlueprintEditorUtils::AddMemberVariable(Blueprint, FName(*AttributeName), PinType);
    if (!bSuccess)
    {
        Ctx.SendError(TEXT("ADD_FAILED"), TEXT("Failed to add attribute"));
        return true;
    }

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
    // Drain any in-flight async package loads before compiling. A prior failed
    // LoadObject of a missing asset can leave the async loader / linker with a
    // pending placeholder; UE 5.4's parallel skeleton compile then collides with
    // that half-loaded state and dereferences null on a background worker
    // (BlueprintCompilationManager SkeletonCompiledBlueprints ensure + AV).
    // Flushing first guarantees the compilation manager starts from a clean state.
    FlushAsyncLoading();
    // Compile so the new attribute materializes as an FProperty on the generated
    // class. Without this, downstream handlers that read the compiled class
    // (gas.set_attribute_base_value, gas.set_execution_capture) cannot see the
    // attribute and fail with ATTRIBUTE_NOT_FOUND until a manual blueprint.compile.
    const BlueprintHandlerUtils::FBlueprintCompileDiagnostics CompileDiagnostics =
        BlueprintHandlerUtils::CompileBlueprintWithDiagnostics(Blueprint);
    McpSafeAssetSave(Blueprint);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("blueprintPath"), BlueprintPath);
    Result->SetStringField(TEXT("attributeName"), AttributeName);
    Result->SetNumberField(TEXT("defaultValue"), DefaultValue);
    BlueprintHandlerUtils::AddCompileDiagnosticsToJson(CompileDiagnostics, Result);
    Ctx.SendSuccess(Result);
    return true;
#endif
}

// ---- gas.set_attribute_base_value ----
REGISTER_RPC_HANDLER("gas.set_attribute_base_value", "gas", "Set the base value of an attribute via reflection",
    RPC_PARAMS(
        RPC_PARAM_REQ("blueprintPath", "path", "Path to the AttributeSet blueprint"),
        RPC_PARAM_REQ("attributeName", "string", "Name of the attribute"),
        RPC_PARAM_DEF("baseValue", "number", "Base value to set", "0")
    ))
{
#if !MCP_HAS_GAS
    Ctx.SendError(TEXT("GAS_NOT_AVAILABLE"), TEXT("GameplayAbilities plugin not enabled."));
    return true;
#else
    FString BlueprintPath = Ctx.GetString(TEXT("blueprintPath"));
    if (BlueprintPath.IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("Missing blueprintPath."));
        return true;
    }

    FString AttributeName = Ctx.GetString(TEXT("attributeName"));
    if (AttributeName.IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("Missing attributeName."));
        return true;
    }

    float BaseValue = static_cast<float>(Ctx.GetNumber(TEXT("baseValue"), 0.0));

    UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *BlueprintPath);
    if (!Blueprint || !Blueprint->GeneratedClass)
    {
        Ctx.SendError(TEXT("NOT_FOUND"), FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintPath));
        return true;
    }

    UAttributeSet* AttrSetCDO = Cast<UAttributeSet>(Blueprint->GeneratedClass->GetDefaultObject());
    if (!AttrSetCDO)
    {
        Ctx.SendError(TEXT("INVALID_TYPE"), TEXT("Not an AttributeSet blueprint"));
        return true;
    }

    UClass* AttrSetClass = Blueprint->GeneratedClass;
    FProperty* AttrProperty = AttrSetClass->FindPropertyByName(FName(*AttributeName));
    if (!AttrProperty)
    {
        Ctx.SendError(TEXT("ATTRIBUTE_NOT_FOUND"), FString::Printf(TEXT("Attribute not found: %s"), *AttributeName));
        return true;
    }

    void* AttrDataPtr = AttrProperty->ContainerPtrToValuePtr<void>(AttrSetCDO);
    if (AttrDataPtr)
    {
        WriteAttributeDataField(AttrDataPtr, TEXT("BaseValue"), static_cast<double>(BaseValue));
        WriteAttributeDataField(AttrDataPtr, TEXT("CurrentValue"), static_cast<double>(BaseValue));
    }

    FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
    AttrSetCDO->MarkPackageDirty();

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("blueprintPath"), BlueprintPath);
    Result->SetStringField(TEXT("attributeName"), AttributeName);
    Result->SetNumberField(TEXT("baseValue"), BaseValue);
    Ctx.SendSuccess(Result);
    return true;
#endif
}

// ============================================================================
// 13.2 GAMEPLAY ABILITIES
// ============================================================================

// ---- gas.create_gameplay_ability ----
REGISTER_RPC_HANDLER("gas.create_gameplay_ability", "gas", "Create a new GameplayAbility blueprint",
    RPC_PARAMS(
        RPC_PARAM_REQ("name", "string", "Name for the ability blueprint"),
        RPC_PARAM_DEF("path", "path", "Content path for the asset", "/Game")
    ))
{
#if !MCP_HAS_GAS
    Ctx.SendError(TEXT("GAS_NOT_AVAILABLE"), TEXT("GameplayAbilities plugin not enabled."));
    return true;
#else
    FString Name = Ctx.GetString(TEXT("name"));
    FString Path = Ctx.GetString(TEXT("path"), TEXT("/Game"));

    if (Name.IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("Missing name."));
        return true;
    }

    FString Error;
    UBlueprint* Blueprint = CreateGASBlueprint(Path, Name, UGameplayAbility::StaticClass(), Error);
    if (!Blueprint)
    {
        Ctx.SendError(TEXT("CREATION_FAILED"), Error);
        return true;
    }

    McpSafeAssetSave(Blueprint);

    FString ActualName = Blueprint->GetName();
    FString ActualPath = Path / ActualName;

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("assetPath"), ActualPath);
    Result->SetStringField(TEXT("name"), ActualName);
    Result->SetStringField(TEXT("parentClass"), TEXT("GameplayAbility"));
    Ctx.SendSuccess(Result);
    return true;
#endif
}

// ---- gas.set_ability_tags ----
REGISTER_RPC_HANDLER("gas.set_ability_tags", "gas", "Set gameplay tags on a GameplayAbility",
    RPC_PARAMS(
        RPC_PARAM_REQ("blueprintPath", "path", "Path to the ability blueprint"),
        RPC_PARAM_OPT("abilityTags", "array", "Array of tag strings to add as ability tags"),
        RPC_PARAM_OPT("cancelAbilitiesWithTags", "array", "Array of tag strings for cancel tags"),
        RPC_PARAM_OPT("blockAbilitiesWithTags", "array", "Array of tag strings for block tags (blocks OTHER abilities with these tags WHILE this ability is active)"),
        RPC_PARAM_OPT("activationBlockedTags", "array", "Array of tag strings that BLOCK THIS ability from activating while the owner has ANY of them (e.g. State.Stunned -> can't cast while stunned). NOT blockAbilitiesWithTags, which is inverted."),
        RPC_PARAM_OPT("activationRequiredTags", "array", "Array of tag strings the owner must have ALL of for this ability to activate"),
        RPC_PARAM_OPT("activationOwnedTags", "array", "Array of tag strings granted to the owner for the duration this ability is active")
    ))
{
#if !MCP_HAS_GAS
    Ctx.SendError(TEXT("GAS_NOT_AVAILABLE"), TEXT("GameplayAbilities plugin not enabled."));
    return true;
#else
    FString BlueprintPath = Ctx.GetString(TEXT("blueprintPath"));
    if (BlueprintPath.IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("Missing blueprintPath."));
        return true;
    }

    UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *BlueprintPath);
    if (!Blueprint || !Blueprint->GeneratedClass)
    {
        Ctx.SendError(TEXT("NOT_FOUND"), FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintPath));
        return true;
    }

    UGameplayAbility* AbilityCDO = Cast<UGameplayAbility>(Blueprint->GeneratedClass->GetDefaultObject());
    if (!AbilityCDO)
    {
        Ctx.SendError(TEXT("INVALID_TYPE"), TEXT("Not a GameplayAbility blueprint"));
        return true;
    }

    auto* Payload = Ctx.GetRawPayload().Get();
    TArray<FString> TagsAdded;

    // Validate-before-mutate across every tag container. Pre-resolve each requested tag
    // against the registry; any unregistered tag (invalid FGameplayTag) is collected into
    // DroppedTags rather than silently skipped. If anything would be dropped, reject the
    // whole call with INVALID_PARAMS + droppedTags — including the cancel/block containers,
    // which previously dropped invisibly (no tagsAdded entry).
    TArray<FGameplayTag> ResolvedAbilityTags;
    TArray<FString> DroppedTags;

    const TArray<TSharedPtr<FJsonValue>>* AbilityTagsArray;
    if (Payload->TryGetArrayField(TEXT("abilityTags"), AbilityTagsArray))
    {
        ResolveTagsInto(*AbilityTagsArray, ResolvedAbilityTags, DroppedTags, &TagsAdded);
    }

    // Reflection-backed tag containers authored by name off the ability CDO. Each row
    // maps its JSON param to the native FGameplayTagContainer UPROPERTY; the resolve,
    // gate, and write below all drive off this one table, so adding a container (GAS
    // also exposes Source*/Target* gating tags in the same idiom) is a single row
    // instead of three hand-synced sites. The activation containers gate owner state
    // ("can't cast while stunned"); note the cancel/block JSON params are plural but
    // their native properties are singular. abilityTags stays special-cased above —
    // 5.7 deprecates it behind GetAssetTags. Every requested tag is resolved through
    // the shared ResolveTagsInto, so an unregistered tag lands in DroppedTags and the
    // whole call is rejected below, never silently imported as an empty container the
    // way the generic blueprint.set_default path would.
    struct FTagContainerParam { const TCHAR* JsonParam; const TCHAR* NativeProp; };
    static const FTagContainerParam TagContainerParams[] = {
        { TEXT("cancelAbilitiesWithTags"), TEXT("CancelAbilitiesWithTag") },
        { TEXT("blockAbilitiesWithTags"),  TEXT("BlockAbilitiesWithTag") },
        { TEXT("activationBlockedTags"),   TEXT("ActivationBlockedTags") },
        { TEXT("activationRequiredTags"),  TEXT("ActivationRequiredTags") },
        { TEXT("activationOwnedTags"),     TEXT("ActivationOwnedTags") },
    };
    const int32 NumTagContainers = UE_ARRAY_COUNT(TagContainerParams);
    TArray<TArray<FGameplayTag>> ResolvedContainers;
    ResolvedContainers.SetNum(NumTagContainers);
    for (int32 i = 0; i < NumTagContainers; ++i)
    {
        const TArray<TSharedPtr<FJsonValue>>* Arr;
        if (Payload->TryGetArrayField(TagContainerParams[i].JsonParam, Arr))
        {
            ResolveTagsInto(*Arr, ResolvedContainers[i], DroppedTags, &TagsAdded);
        }
    }

    if (DroppedTags.Num() > 0)
    {
        Ctx.SendError(TEXT("INVALID_PARAMS"),
            FString::Printf(TEXT("%d ability tag(s) are not registered and would be dropped; register them first (e.g. via gameplay_tags.add) then retry. See droppedTags."), DroppedTags.Num()),
            MakeDroppedTagsErrData(DroppedTags));
        return true;
    }

    // All tags resolved: apply them. On 5.7+ AbilityTags is deprecated and the
    // container is read via GetAssetTags(), so snapshot once, accumulate all tags
    // into the copy, then assign back a single time (avoids an O(N^2) per-tag copy/
    // reassign of the whole container). Pre-5.7 AddTag is in-place on AbilityTags.
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 7, 0)
    {
        FGameplayTagContainer CurrentTags = AbilityCDO->GetAssetTags();
        for (const FGameplayTag& Tag : ResolvedAbilityTags)
        {
            CurrentTags.AddTag(Tag);
        }
        PRAGMA_DISABLE_DEPRECATION_WARNINGS
        AbilityCDO->AbilityTags = CurrentTags;
        PRAGMA_ENABLE_DEPRECATION_WARNINGS
    }
#else
    for (const FGameplayTag& Tag : ResolvedAbilityTags)
    {
        PRAGMA_DISABLE_DEPRECATION_WARNINGS
        AbilityCDO->AbilityTags.AddTag(Tag);
        PRAGMA_ENABLE_DEPRECATION_WARNINGS
    }
#endif
    for (int32 i = 0; i < NumTagContainers; ++i)
    {
        const FName NativeProp(TagContainerParams[i].NativeProp);
        for (const FGameplayTag& Tag : ResolvedContainers[i])
        {
            AddTagToAbilityContainer(AbilityCDO, NativeProp, Tag);
        }
    }

    FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("blueprintPath"), BlueprintPath);
    Result->SetArrayField(TEXT("tagsAdded"), EmitStringArray(TagsAdded));
    Ctx.SendSuccess(Result);
    return true;
#endif
}

// ---- gas.set_ability_costs ----
REGISTER_RPC_HANDLER("gas.set_ability_costs", "gas", "Set the cost gameplay effect for an ability",
    RPC_PARAMS(
        RPC_PARAM_REQ("blueprintPath", "path", "Path to the ability blueprint"),
        RPC_PARAM_OPT("costEffectPath", "classref", "Path to the cost GameplayEffect class")
    ))
{
#if !MCP_HAS_GAS
    Ctx.SendError(TEXT("GAS_NOT_AVAILABLE"), TEXT("GameplayAbilities plugin not enabled."));
    return true;
#else
    FString BlueprintPath = Ctx.GetString(TEXT("blueprintPath"));
    if (BlueprintPath.IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("Missing blueprintPath."));
        return true;
    }

    FString CostEffectPath = Ctx.GetString(TEXT("costEffectPath"));

    UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *BlueprintPath);
    if (!Blueprint || !Blueprint->GeneratedClass)
    {
        Ctx.SendError(TEXT("NOT_FOUND"), FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintPath));
        return true;
    }

    UGameplayAbility* AbilityCDO = Cast<UGameplayAbility>(Blueprint->GeneratedClass->GetDefaultObject());
    if (!AbilityCDO)
    {
        Ctx.SendError(TEXT("INVALID_TYPE"), TEXT("Not a GameplayAbility blueprint"));
        return true;
    }

    if (!CostEffectPath.IsEmpty())
    {
        TSubclassOf<UGameplayEffect> CostClass;
        if (!ResolveGameplayEffectClassArg(Ctx, CostEffectPath, TEXT("Cost"), CostClass))
        {
            return true;
        }
        SetAbilityPropertyValue(AbilityCDO, FName(TEXT("CostGameplayEffectClass")), CostClass);
    }

    FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("blueprintPath"), BlueprintPath);
    Result->SetStringField(TEXT("costEffectPath"), CostEffectPath);
    Ctx.SendSuccess(Result);
    return true;
#endif
}

// ---- gas.set_ability_cooldown ----
REGISTER_RPC_HANDLER("gas.set_ability_cooldown", "gas", "Set the cooldown gameplay effect for an ability",
    RPC_PARAMS(
        RPC_PARAM_REQ("blueprintPath", "path", "Path to the ability blueprint"),
        RPC_PARAM_OPT("cooldownEffectPath", "classref", "Path to the cooldown GameplayEffect class")
    ))
{
#if !MCP_HAS_GAS
    Ctx.SendError(TEXT("GAS_NOT_AVAILABLE"), TEXT("GameplayAbilities plugin not enabled."));
    return true;
#else
    FString BlueprintPath = Ctx.GetString(TEXT("blueprintPath"));
    if (BlueprintPath.IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("Missing blueprintPath."));
        return true;
    }

    FString CooldownEffectPath = Ctx.GetString(TEXT("cooldownEffectPath"));

    UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *BlueprintPath);
    if (!Blueprint || !Blueprint->GeneratedClass)
    {
        Ctx.SendError(TEXT("NOT_FOUND"), FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintPath));
        return true;
    }

    UGameplayAbility* AbilityCDO = Cast<UGameplayAbility>(Blueprint->GeneratedClass->GetDefaultObject());
    if (!AbilityCDO)
    {
        Ctx.SendError(TEXT("INVALID_TYPE"), TEXT("Not a GameplayAbility blueprint"));
        return true;
    }

    if (!CooldownEffectPath.IsEmpty())
    {
        TSubclassOf<UGameplayEffect> CooldownClass;
        if (!ResolveGameplayEffectClassArg(Ctx, CooldownEffectPath, TEXT("Cooldown"), CooldownClass))
        {
            return true;
        }
        SetAbilityPropertyValue(AbilityCDO, FName(TEXT("CooldownGameplayEffectClass")), CooldownClass);
    }

    FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("blueprintPath"), BlueprintPath);
    Result->SetStringField(TEXT("cooldownEffectPath"), CooldownEffectPath);
    Ctx.SendSuccess(Result);
    return true;
#endif
}

// ---- gas.set_activation_policy ----
REGISTER_RPC_HANDLER("gas.set_activation_policy", "gas", "Set the network execution policy for an ability",
    RPC_PARAMS(
        RPC_PARAM_REQ("blueprintPath", "path", "Path to the ability blueprint"),
        RPC_PARAM_DEF("policy", "string", "Policy: local_only, local_predicted, server_only, server_initiated", "local_predicted")
    ))
{
#if !MCP_HAS_GAS
    Ctx.SendError(TEXT("GAS_NOT_AVAILABLE"), TEXT("GameplayAbilities plugin not enabled."));
    return true;
#else
    FString BlueprintPath = Ctx.GetString(TEXT("blueprintPath"));
    if (BlueprintPath.IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("Missing blueprintPath."));
        return true;
    }

    FString Policy = Ctx.GetString(TEXT("policy"), TEXT("local_predicted"));

    UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *BlueprintPath);
    if (!Blueprint || !Blueprint->GeneratedClass)
    {
        Ctx.SendError(TEXT("NOT_FOUND"), FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintPath));
        return true;
    }

    UGameplayAbility* AbilityCDO = Cast<UGameplayAbility>(Blueprint->GeneratedClass->GetDefaultObject());
    if (!AbilityCDO)
    {
        Ctx.SendError(TEXT("INVALID_TYPE"), TEXT("Not a GameplayAbility blueprint"));
        return true;
    }

    TEnumAsByte<EGameplayAbilityNetExecutionPolicy::Type> NetPolicy;
    if (Policy == TEXT("local_only"))
    {
        NetPolicy = EGameplayAbilityNetExecutionPolicy::LocalOnly;
    }
    else if (Policy == TEXT("local_predicted"))
    {
        NetPolicy = EGameplayAbilityNetExecutionPolicy::LocalPredicted;
    }
    else if (Policy == TEXT("server_only"))
    {
        NetPolicy = EGameplayAbilityNetExecutionPolicy::ServerOnly;
    }
    else if (Policy == TEXT("server_initiated"))
    {
        NetPolicy = EGameplayAbilityNetExecutionPolicy::ServerInitiated;
    }
    else
    {
        NetPolicy = EGameplayAbilityNetExecutionPolicy::LocalPredicted;
    }
    SetAbilityPropertyValue(AbilityCDO, FName(TEXT("NetExecutionPolicy")), NetPolicy);

    FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("blueprintPath"), BlueprintPath);
    Result->SetStringField(TEXT("policy"), Policy);
    Ctx.SendSuccess(Result);
    return true;
#endif
}

// ---- gas.set_instancing_policy ----
REGISTER_RPC_HANDLER("gas.set_instancing_policy", "gas", "Set the instancing policy for an ability",
    RPC_PARAMS(
        RPC_PARAM_REQ("blueprintPath", "path", "Path to the ability blueprint"),
        RPC_PARAM_DEF("policy", "string", "Policy: non_instanced, instanced_per_actor, instanced_per_execution", "instanced_per_actor")
    ))
{
#if !MCP_HAS_GAS
    Ctx.SendError(TEXT("GAS_NOT_AVAILABLE"), TEXT("GameplayAbilities plugin not enabled."));
    return true;
#else
    FString BlueprintPath = Ctx.GetString(TEXT("blueprintPath"));
    if (BlueprintPath.IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("Missing blueprintPath."));
        return true;
    }

    FString Policy = Ctx.GetString(TEXT("policy"), TEXT("instanced_per_actor"));

    UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *BlueprintPath);
    if (!Blueprint || !Blueprint->GeneratedClass)
    {
        Ctx.SendError(TEXT("NOT_FOUND"), FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintPath));
        return true;
    }

    UGameplayAbility* AbilityCDO = Cast<UGameplayAbility>(Blueprint->GeneratedClass->GetDefaultObject());
    if (!AbilityCDO)
    {
        Ctx.SendError(TEXT("INVALID_TYPE"), TEXT("Not a GameplayAbility blueprint"));
        return true;
    }

    TEnumAsByte<EGameplayAbilityInstancingPolicy::Type> InstPolicy;
    if (Policy == TEXT("non_instanced"))
    {
        PRAGMA_DISABLE_DEPRECATION_WARNINGS
        InstPolicy = EGameplayAbilityInstancingPolicy::NonInstanced;
        PRAGMA_ENABLE_DEPRECATION_WARNINGS
    }
    else if (Policy == TEXT("instanced_per_actor"))
    {
        InstPolicy = EGameplayAbilityInstancingPolicy::InstancedPerActor;
    }
    else if (Policy == TEXT("instanced_per_execution"))
    {
        InstPolicy = EGameplayAbilityInstancingPolicy::InstancedPerExecution;
    }
    else
    {
        InstPolicy = EGameplayAbilityInstancingPolicy::InstancedPerActor;
    }
    SetAbilityPropertyValue(AbilityCDO, FName(TEXT("InstancingPolicy")), InstPolicy);

    FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("blueprintPath"), BlueprintPath);
    Result->SetStringField(TEXT("policy"), Policy);
    Ctx.SendSuccess(Result);
    return true;
#endif
}

// ============================================================================
// 13.3 GAMEPLAY EFFECTS
// ============================================================================

// ---- gas.create_gameplay_effect ----
REGISTER_RPC_HANDLER("gas.create_gameplay_effect", "gas", "Create a new GameplayEffect blueprint",
    RPC_PARAMS(
        RPC_PARAM_REQ("name", "string", "Name for the effect blueprint"),
        RPC_PARAM_DEF("path", "path", "Content path for the asset", "/Game"),
        RPC_PARAM_DEF("durationType", "string", "Duration type: instant, infinite, has_duration", "instant")
    ))
{
#if !MCP_HAS_GAS
    Ctx.SendError(TEXT("GAS_NOT_AVAILABLE"), TEXT("GameplayAbilities plugin not enabled."));
    return true;
#else
    FString Name = Ctx.GetString(TEXT("name"));
    FString Path = Ctx.GetString(TEXT("path"), TEXT("/Game"));

    if (Name.IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("Missing name."));
        return true;
    }

    FString Error;
    UBlueprint* Blueprint = CreateGASBlueprint(Path, Name, UGameplayEffect::StaticClass(), Error);
    if (!Blueprint)
    {
        Ctx.SendError(TEXT("CREATION_FAILED"), Error);
        return true;
    }

    FString DurationType = Ctx.GetString(TEXT("durationType"), TEXT("instant"));

    if (Blueprint->GeneratedClass)
    {
        UGameplayEffect* EffectCDO = Cast<UGameplayEffect>(Blueprint->GeneratedClass->GetDefaultObject());
        if (EffectCDO)
        {
            if (DurationType == TEXT("instant"))
            {
                EffectCDO->DurationPolicy = EGameplayEffectDurationType::Instant;
            }
            else if (DurationType == TEXT("infinite"))
            {
                EffectCDO->DurationPolicy = EGameplayEffectDurationType::Infinite;
            }
            else if (DurationType == TEXT("has_duration"))
            {
                EffectCDO->DurationPolicy = EGameplayEffectDurationType::HasDuration;
            }
        }
    }

    McpSafeAssetSave(Blueprint);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("assetPath"), Path / Name);
    Result->SetStringField(TEXT("name"), Name);
    Result->SetStringField(TEXT("parentClass"), TEXT("GameplayEffect"));
    Result->SetStringField(TEXT("durationType"), DurationType);
    Ctx.SendSuccess(Result);
    return true;
#endif
}

// ---- gas.set_effect_duration ----
REGISTER_RPC_HANDLER("gas.set_effect_duration", "gas", "Set the duration policy and magnitude for a GameplayEffect",
    RPC_PARAMS(
        RPC_PARAM_REQ("blueprintPath", "path", "Path to the effect blueprint"),
        RPC_PARAM_DEF("durationType", "string", "Duration type: instant, infinite, has_duration", "instant"),
        RPC_PARAM_DEF("duration", "number", "Duration in seconds (for has_duration)", "0")
    ))
{
#if !MCP_HAS_GAS
    Ctx.SendError(TEXT("GAS_NOT_AVAILABLE"), TEXT("GameplayAbilities plugin not enabled."));
    return true;
#else
    FString BlueprintPath = Ctx.GetString(TEXT("blueprintPath"));
    if (BlueprintPath.IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("Missing blueprintPath."));
        return true;
    }

    UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *BlueprintPath);
    if (!Blueprint || !Blueprint->GeneratedClass)
    {
        Ctx.SendError(TEXT("NOT_FOUND"), FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintPath));
        return true;
    }

    UGameplayEffect* EffectCDO = Cast<UGameplayEffect>(Blueprint->GeneratedClass->GetDefaultObject());
    if (!EffectCDO)
    {
        Ctx.SendError(TEXT("INVALID_TYPE"), TEXT("Not a GameplayEffect blueprint"));
        return true;
    }

    FString DurationType = Ctx.GetString(TEXT("durationType"), TEXT("instant"));
    float Duration = static_cast<float>(Ctx.GetNumber(TEXT("duration"), 0.0));

    if (DurationType == TEXT("instant"))
    {
        EffectCDO->DurationPolicy = EGameplayEffectDurationType::Instant;
    }
    else if (DurationType == TEXT("infinite"))
    {
        EffectCDO->DurationPolicy = EGameplayEffectDurationType::Infinite;
    }
    else if (DurationType == TEXT("has_duration"))
    {
        EffectCDO->DurationPolicy = EGameplayEffectDurationType::HasDuration;
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 7, 0)
        EffectCDO->DurationMagnitude = FGameplayEffectModifierMagnitude(FScalableFloat(Duration));
#else
        EffectCDO->DurationMagnitude = FGameplayEffectModifierMagnitude(FScalableFloat(Duration));
#endif
    }

    FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("blueprintPath"), BlueprintPath);
    Result->SetStringField(TEXT("durationType"), DurationType);
    Result->SetNumberField(TEXT("duration"), Duration);
    Ctx.SendSuccess(Result);
    return true;
#endif
}

// ---- gas.add_effect_modifier ----
REGISTER_RPC_HANDLER("gas.add_effect_modifier", "gas", "Add a modifier to a GameplayEffect",
    RPC_PARAMS(
        RPC_PARAM_REQ("blueprintPath", "path", "Path to the effect blueprint"),
        RPC_PARAM_DEF("operation", "string", "Operation: additive, multiplicative, division, override", "additive"),
        RPC_PARAM_DEF("magnitude", "number", "Modifier magnitude value", "0"),
        // Optional attribute binding; 'attributeName' is accepted as a dispatcher-level alias.
        FParamSpec{TEXT("attribute"), TEXT("classref"),
            TEXT("Attribute this modifier targets, in 'AttributeSetClassPath_C.AttrName' form (e.g. '/Game/GAS/BP_MyAttributeSet_C.AttackPower'). Omit to leave the modifier unbound."),
            false, TEXT(""), {TEXT("attributeName")}}
    ))
{
#if !MCP_HAS_GAS
    Ctx.SendError(TEXT("GAS_NOT_AVAILABLE"), TEXT("GameplayAbilities plugin not enabled."));
    return true;
#else
    FString BlueprintPath = Ctx.GetString(TEXT("blueprintPath"));
    if (BlueprintPath.IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("Missing blueprintPath."));
        return true;
    }

    UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *BlueprintPath);
    if (!Blueprint || !Blueprint->GeneratedClass)
    {
        Ctx.SendError(TEXT("NOT_FOUND"), FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintPath));
        return true;
    }

    UGameplayEffect* EffectCDO = Cast<UGameplayEffect>(Blueprint->GeneratedClass->GetDefaultObject());
    if (!EffectCDO)
    {
        Ctx.SendError(TEXT("INVALID_TYPE"), TEXT("Not a GameplayEffect blueprint"));
        return true;
    }

    FString Operation = Ctx.GetString(TEXT("operation"), TEXT("additive"));
    float Magnitude = static_cast<float>(Ctx.GetNumber(TEXT("magnitude"), 0.0));

    FGameplayModifierInfo Modifier;

    if (Operation == TEXT("additive") || Operation == TEXT("add"))
    {
        Modifier.ModifierOp = EGameplayModOp::Additive;
    }
    else if (Operation == TEXT("multiplicative") || Operation == TEXT("multiply"))
    {
        Modifier.ModifierOp = EGameplayModOp::Multiplicitive;
    }
    else if (Operation == TEXT("division") || Operation == TEXT("divide"))
    {
        Modifier.ModifierOp = EGameplayModOp::Division;
    }
    else if (Operation == TEXT("override"))
    {
        Modifier.ModifierOp = EGameplayModOp::Override;
    }

    Modifier.ModifierMagnitude = FGameplayEffectModifierMagnitude(FScalableFloat(Magnitude));

    // Optional: bind the modifier to the attribute it targets. Without this the
    // modifier lands with an empty FGameplayAttribute and is inert (modifies nothing).
    // Accept both 'attribute' and the 'attributeName' alias.
    FString AttrSpec = Ctx.GetStringFirstOf({TEXT("attribute"), TEXT("attributeName")});
    FString ResolvedAttribute;
    if (!AttrSpec.IsEmpty())
    {
        FGameplayAttribute Attr;
        FString AttrErrCode, AttrErrMsg;
        if (!ResolveGameplayAttributeFromSpec(AttrSpec, Attr, AttrErrCode, AttrErrMsg))
        {
            Ctx.SendError(AttrErrCode, AttrErrMsg);
            return true;
        }
        Modifier.Attribute = Attr;
        ResolvedAttribute = Attr.GetName();
    }

    EffectCDO->Modifiers.Add(Modifier);

    FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("blueprintPath"), BlueprintPath);
    Result->SetStringField(TEXT("operation"), Operation);
    Result->SetNumberField(TEXT("magnitude"), Magnitude);
    Result->SetNumberField(TEXT("modifierCount"), EffectCDO->Modifiers.Num());
    // Echo the resolved attribute back so callers can confirm the binding.
    Result->SetStringField(TEXT("attribute"), ResolvedAttribute);
    Ctx.SendSuccess(Result);
    return true;
#endif
}

// ---- gas.set_modifier_magnitude ----
REGISTER_RPC_HANDLER("gas.set_modifier_magnitude", "gas", "Set the magnitude of an existing modifier on a GameplayEffect",
    RPC_PARAMS(
        RPC_PARAM_REQ("blueprintPath", "path", "Path to the effect blueprint"),
        RPC_PARAM_DEF("modifierIndex", "integer", "Index of the modifier to update", "0"),
        RPC_PARAM_DEF("value", "number", "New magnitude value", "0"),
        RPC_PARAM_DEF("magnitudeType", "string", "Magnitude type: scalable_float", "scalable_float")
    ))
{
#if !MCP_HAS_GAS
    Ctx.SendError(TEXT("GAS_NOT_AVAILABLE"), TEXT("GameplayAbilities plugin not enabled."));
    return true;
#else
    UBlueprint* Blueprint = nullptr;
    UGameplayEffect* EffectCDO = nullptr;
    int32 ModifierIndex = 0;
    if (!ResolveEffectAndModifier(Ctx, Blueprint, EffectCDO, ModifierIndex))
    {
        return true;
    }

    float Value = static_cast<float>(Ctx.GetNumber(TEXT("value"), 0.0));
    FString MagnitudeType = Ctx.GetString(TEXT("magnitudeType"), TEXT("scalable_float"));

    EffectCDO->Modifiers[ModifierIndex].ModifierMagnitude = FGameplayEffectModifierMagnitude(FScalableFloat(Value));

    FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("blueprintPath"), Ctx.GetString(TEXT("blueprintPath")));
    Result->SetNumberField(TEXT("modifierIndex"), ModifierIndex);
    Result->SetStringField(TEXT("magnitudeType"), MagnitudeType);
    Result->SetNumberField(TEXT("value"), Value);
    Ctx.SendSuccess(Result);
    return true;
#endif
}

// ---- gas.set_modifier_attribute ----
REGISTER_RPC_HANDLER("gas.set_modifier_attribute", "gas", "Bind an existing GameplayEffect modifier to the attribute it modifies",
    RPC_PARAMS(
        RPC_PARAM_REQ("blueprintPath", "path", "Path to the effect blueprint"),
        RPC_PARAM_DEF("modifierIndex", "integer", "Index of the modifier to update", "0"),
        // 'attributeName' is accepted as a dispatcher-level alias for 'attribute'.
        FParamSpec{TEXT("attribute"), TEXT("classref"),
            TEXT("Attribute this modifier targets, in 'AttributeSetClassPath_C.AttrName' form (e.g. '/Game/GAS/BP_MyAttributeSet_C.AttackPower')"),
            true, TEXT(""), {TEXT("attributeName")}}
    ))
{
#if !MCP_HAS_GAS
    Ctx.SendError(TEXT("GAS_NOT_AVAILABLE"), TEXT("GameplayAbilities plugin not enabled."));
    return true;
#else
    UBlueprint* Blueprint = nullptr;
    UGameplayEffect* EffectCDO = nullptr;
    int32 ModifierIndex = 0;
    if (!ResolveEffectAndModifier(Ctx, Blueprint, EffectCDO, ModifierIndex))
    {
        return true;
    }

    // 'attribute' is required, but the dispatcher's HasField check lets a present-but-empty
    // value through; guard that edge here (an absent key is already MISSING_REQUIRED_PARAM).
    FString AttrSpec = Ctx.GetStringFirstOf({TEXT("attribute"), TEXT("attributeName")});
    if (AttrSpec.IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("Missing attribute."));
        return true;
    }

    FGameplayAttribute Attr;
    FString AttrErrCode, AttrErrMsg;
    if (!ResolveGameplayAttributeFromSpec(AttrSpec, Attr, AttrErrCode, AttrErrMsg))
    {
        Ctx.SendError(AttrErrCode, AttrErrMsg);
        return true;
    }

    EffectCDO->Modifiers[ModifierIndex].Attribute = Attr;

    FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("blueprintPath"), Ctx.GetString(TEXT("blueprintPath")));
    Result->SetNumberField(TEXT("modifierIndex"), ModifierIndex);
    Result->SetStringField(TEXT("attribute"), Attr.GetName());
    Ctx.SendSuccess(Result);
    return true;
#endif
}

// ---- gas.add_effect_execution_calculation ----
REGISTER_RPC_HANDLER("gas.add_effect_execution_calculation", "gas", "Add an execution calculation to a GameplayEffect",
    RPC_PARAMS(
        RPC_PARAM_REQ("blueprintPath", "path", "Path to the effect blueprint"),
        RPC_PARAM_REQ("calculationClass", "classref", "Path to the calculation class")
    ))
{
#if !MCP_HAS_GAS
    Ctx.SendError(TEXT("GAS_NOT_AVAILABLE"), TEXT("GameplayAbilities plugin not enabled."));
    return true;
#else
    FString BlueprintPath = Ctx.GetString(TEXT("blueprintPath"));
    if (BlueprintPath.IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("Missing blueprintPath."));
        return true;
    }

    FString CalculationClassPath = Ctx.GetString(TEXT("calculationClass"));
    if (CalculationClassPath.IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("Missing calculationClass."));
        return true;
    }

    UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *BlueprintPath);
    if (!Blueprint || !Blueprint->GeneratedClass)
    {
        Ctx.SendError(TEXT("NOT_FOUND"), FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintPath));
        return true;
    }

    UGameplayEffect* EffectCDO = Cast<UGameplayEffect>(Blueprint->GeneratedClass->GetDefaultObject());
    if (!EffectCDO)
    {
        Ctx.SendError(TEXT("INVALID_TYPE"), TEXT("Not a GameplayEffect blueprint"));
        return true;
    }

    // Route the calc-class param through the shared validate-before-mutate
    // resolver so the bare blueprint-asset path (/Game/.../DamageMitigationExec —
    // exactly what gas.create_execution_calculation returns as its assetPath)
    // resolves to the generated class without a hand-appended _C suffix, matching
    // the resolver contract from E-class-name-format-inconsistency. Keep
    // CLASS_NOT_FOUND on a miss for backward compat and reject a non-exec-calc
    // class with INVALID_TYPE before the downstream cast
    // (E-gas-add-execution-calc-requires-c-suffix).
    TSubclassOf<UGameplayEffectExecutionCalculation> CalcClass;
    if (!ResolveGasSubclassArg<UGameplayEffectExecutionCalculation>(Ctx, CalculationClassPath,
            TEXT("CLASS_NOT_FOUND"), TEXT("Calculation class"),
            TEXT("GameplayEffectExecutionCalculation class"), CalcClass))
    {
        return true;
    }

    FGameplayEffectExecutionDefinition ExecDef;
    ExecDef.CalculationClass = CalcClass;
    EffectCDO->Executions.Add(ExecDef);

    FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("blueprintPath"), BlueprintPath);
    Result->SetStringField(TEXT("calculationClass"), CalculationClassPath);
    Result->SetNumberField(TEXT("executionCount"), EffectCDO->Executions.Num());
    Ctx.SendSuccess(Result);
    return true;
#endif
}

// ---- gas.add_effect_cue ----
REGISTER_RPC_HANDLER("gas.add_effect_cue", "gas", "Add a gameplay cue to a GameplayEffect",
    RPC_PARAMS(
        RPC_PARAM_REQ("blueprintPath", "path", "Path to the effect blueprint"),
        RPC_PARAM_REQ("cueTag", "string", "Gameplay cue tag string")
    ))
{
#if !MCP_HAS_GAS
    Ctx.SendError(TEXT("GAS_NOT_AVAILABLE"), TEXT("GameplayAbilities plugin not enabled."));
    return true;
#else
    FString BlueprintPath = Ctx.GetString(TEXT("blueprintPath"));
    if (BlueprintPath.IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("Missing blueprintPath."));
        return true;
    }

    FString CueTag = Ctx.GetString(TEXT("cueTag"));
    if (CueTag.IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("Missing cueTag."));
        return true;
    }

    UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *BlueprintPath);
    if (!Blueprint || !Blueprint->GeneratedClass)
    {
        Ctx.SendError(TEXT("NOT_FOUND"), FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintPath));
        return true;
    }

    UGameplayEffect* EffectCDO = Cast<UGameplayEffect>(Blueprint->GeneratedClass->GetDefaultObject());
    if (!EffectCDO)
    {
        Ctx.SendError(TEXT("INVALID_TYPE"), TEXT("Not a GameplayEffect blueprint"));
        return true;
    }

    FGameplayTag Tag = GetOrRequestTag(CueTag);
    if (Tag.IsValid())
    {
        FGameplayEffectCue Cue;
        Cue.GameplayCueTags.AddTag(Tag);
        EffectCDO->GameplayCues.Add(Cue);
    }

    FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("blueprintPath"), BlueprintPath);
    Result->SetStringField(TEXT("cueTag"), CueTag);
    Result->SetNumberField(TEXT("cueCount"), EffectCDO->GameplayCues.Num());
    Ctx.SendSuccess(Result);
    return true;
#endif
}

// ---- gas.set_effect_stacking ----
REGISTER_RPC_HANDLER("gas.set_effect_stacking", "gas", "Configure stacking behavior for a GameplayEffect",
    RPC_PARAMS(
        RPC_PARAM_REQ("blueprintPath", "path", "Path to the effect blueprint"),
        RPC_PARAM_DEF("stackingType", "string", "Stacking type: none, aggregate_by_source, aggregate_by_target", "none"),
        RPC_PARAM_DEF("stackLimit", "number", "Maximum stack count", "1")
    ))
{
#if !MCP_HAS_GAS
    Ctx.SendError(TEXT("GAS_NOT_AVAILABLE"), TEXT("GameplayAbilities plugin not enabled."));
    return true;
#else
    FString BlueprintPath = Ctx.GetString(TEXT("blueprintPath"));
    if (BlueprintPath.IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("Missing blueprintPath."));
        return true;
    }

    UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *BlueprintPath);
    if (!Blueprint || !Blueprint->GeneratedClass)
    {
        Ctx.SendError(TEXT("NOT_FOUND"), FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintPath));
        return true;
    }

    UGameplayEffect* EffectCDO = Cast<UGameplayEffect>(Blueprint->GeneratedClass->GetDefaultObject());
    if (!EffectCDO)
    {
        Ctx.SendError(TEXT("INVALID_TYPE"), TEXT("Not a GameplayEffect blueprint"));
        return true;
    }

    FString StackingType = Ctx.GetString(TEXT("stackingType"), TEXT("none"));
    int32 StackLimit = Ctx.GetInt(TEXT("stackLimit"), 1);

    if (StackingType == TEXT("none"))
    {
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 7, 0)
        PRAGMA_DISABLE_DEPRECATION_WARNINGS
#endif
        EffectCDO->StackingType = EGameplayEffectStackingType::None;
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 7, 0)
        PRAGMA_ENABLE_DEPRECATION_WARNINGS
#endif
    }
    else if (StackingType == TEXT("aggregate_by_source"))
    {
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 7, 0)
        PRAGMA_DISABLE_DEPRECATION_WARNINGS
#endif
        EffectCDO->StackingType = EGameplayEffectStackingType::AggregateBySource;
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 7, 0)
        PRAGMA_ENABLE_DEPRECATION_WARNINGS
#endif
    }
    else if (StackingType == TEXT("aggregate_by_target"))
    {
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 7, 0)
        PRAGMA_DISABLE_DEPRECATION_WARNINGS
#endif
        EffectCDO->StackingType = EGameplayEffectStackingType::AggregateByTarget;
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 7, 0)
        PRAGMA_ENABLE_DEPRECATION_WARNINGS
#endif
    }

    EffectCDO->StackLimitCount = StackLimit;

    FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("blueprintPath"), BlueprintPath);
    Result->SetStringField(TEXT("stackingType"), StackingType);
    Result->SetNumberField(TEXT("stackLimit"), StackLimit);
    Ctx.SendSuccess(Result);
    return true;
#endif
}

// ---- gas.set_effect_tags ----
REGISTER_RPC_HANDLER("gas.set_effect_tags", "gas", "Set granted tags on a GameplayEffect",
    RPC_PARAMS(
        RPC_PARAM_REQ("blueprintPath", "path", "Path to the effect blueprint"),
        RPC_PARAM_OPT("grantedTags", "array", "Array of tag strings to grant")
    ))
{
#if !MCP_HAS_GAS
    Ctx.SendError(TEXT("GAS_NOT_AVAILABLE"), TEXT("GameplayAbilities plugin not enabled."));
    return true;
#else
    FString BlueprintPath = Ctx.GetString(TEXT("blueprintPath"));
    if (BlueprintPath.IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("Missing blueprintPath."));
        return true;
    }

    UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *BlueprintPath);
    if (!Blueprint || !Blueprint->GeneratedClass)
    {
        Ctx.SendError(TEXT("NOT_FOUND"), FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintPath));
        return true;
    }

    UGameplayEffect* EffectCDO = Cast<UGameplayEffect>(Blueprint->GeneratedClass->GetDefaultObject());
    if (!EffectCDO)
    {
        Ctx.SendError(TEXT("INVALID_TYPE"), TEXT("Not a GameplayEffect blueprint"));
        return true;
    }

    auto* Payload = Ctx.GetRawPayload().Get();
    TArray<FString> TagsAdded;

    // Validate-before-mutate: pre-resolve every requested tag against the registry
    // and collect any that do not resolve. An unregistered tag yields an invalid
    // FGameplayTag; rather than silently dropping it (misleading success), reject the
    // whole call with INVALID_PARAMS + a droppedTags list so the caller can register
    // them first via gameplay_tags.add. Matches the convention in ai.configure_slot_behavior.
    TArray<FGameplayTag> ResolvedTags;
    TArray<FString> DroppedTags;
    const TArray<TSharedPtr<FJsonValue>>* GrantedTagsArray;
    if (Payload->TryGetArrayField(TEXT("grantedTags"), GrantedTagsArray))
    {
        ResolveTagsInto(*GrantedTagsArray, ResolvedTags, DroppedTags, &TagsAdded);
    }

    if (DroppedTags.Num() > 0)
    {
        Ctx.SendError(TEXT("INVALID_PARAMS"),
            FString::Printf(TEXT("%d granted tag(s) are not registered and would be dropped; register them first (e.g. via gameplay_tags.add) then retry. See droppedTags."), DroppedTags.Num()),
            MakeDroppedTagsErrData(DroppedTags));
        return true;
    }

    // All tags resolved: apply them through the UE 5.3+ component model so the GE
    // actually grants the tags (GetGrantedTags() / the cooldown validator read the
    // component, not the deprecated container). GrantTagsOnEffect also keeps the
    // deprecated container in sync for back-compat.
    GrantTagsOnEffect(EffectCDO, ResolvedTags);

    FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("blueprintPath"), BlueprintPath);
    Result->SetArrayField(TEXT("tagsAdded"), EmitStringArray(TagsAdded));
    Ctx.SendSuccess(Result);
    return true;
#endif
}

// ============================================================================
// 13.4 GAMEPLAY CUES
// ============================================================================

// ---- gas.create_gameplay_cue_notify ----
REGISTER_RPC_HANDLER("gas.create_gameplay_cue_notify", "gas", "Create a gameplay cue notify blueprint (static or actor)",
    RPC_PARAMS(
        RPC_PARAM_REQ("name", "string", "Name for the cue notify blueprint"),
        RPC_PARAM_DEF("path", "path", "Content path for the asset", "/Game"),
        RPC_PARAM_DEF("cueType", "string", "Cue type: static or actor", "static"),
        RPC_PARAM_OPT("cueTag", "string", "Gameplay cue tag to assign")
    ))
{
#if !MCP_HAS_GAS
    Ctx.SendError(TEXT("GAS_NOT_AVAILABLE"), TEXT("GameplayAbilities plugin not enabled."));
    return true;
#else
    FString Name = Ctx.GetString(TEXT("name"));
    FString Path = Ctx.GetString(TEXT("path"), TEXT("/Game"));

    if (Name.IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("Missing name."));
        return true;
    }

    FString CueType = Ctx.GetString(TEXT("cueType"), TEXT("static"));
    FString CueTag = Ctx.GetString(TEXT("cueTag"));

    UClass* ParentClass = (CueType == TEXT("actor"))
        ? AGameplayCueNotify_Actor::StaticClass()
        : UGameplayCueNotify_Static::StaticClass();

    FString Error;
    UBlueprint* Blueprint = CreateGASBlueprint(Path, Name, ParentClass, Error);
    if (!Blueprint)
    {
        Ctx.SendError(TEXT("CREATION_FAILED"), Error);
        return true;
    }

    if (!CueTag.IsEmpty() && Blueprint->GeneratedClass)
    {
        FGameplayTag Tag = GetOrRequestTag(CueTag);

        if (CueType == TEXT("static"))
        {
            UGameplayCueNotify_Static* CueCDO = Cast<UGameplayCueNotify_Static>(
                Blueprint->GeneratedClass->GetDefaultObject());
            if (CueCDO)
            {
                CueCDO->GameplayCueTag = Tag;
            }
        }
        else
        {
            AGameplayCueNotify_Actor* CueCDO = Cast<AGameplayCueNotify_Actor>(
                Blueprint->GeneratedClass->GetDefaultObject());
            if (CueCDO)
            {
                CueCDO->GameplayCueTag = Tag;
            }
        }
    }

    McpSafeAssetSave(Blueprint);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("assetPath"), Path / Name);
    Result->SetStringField(TEXT("name"), Name);
    Result->SetStringField(TEXT("cueType"), CueType);
    Result->SetStringField(TEXT("cueTag"), CueTag);
    Ctx.SendSuccess(Result);
    return true;
#endif
}

// ============================================================================
// 13.5 TAGS / UTILITY
// ============================================================================

// ---- gas.add_tag_to_asset ----
REGISTER_RPC_HANDLER("gas.add_tag_to_asset", "gas", "Add a gameplay tag to a GAS asset",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath", "path", "Path to the GAS asset"),
        RPC_PARAM_REQ("tag", "string", "Gameplay tag string to add")
    ))
{
#if !MCP_HAS_GAS
    Ctx.SendError(TEXT("GAS_NOT_AVAILABLE"), TEXT("GameplayAbilities plugin not enabled."));
    return true;
#else
    FString AssetPath = Ctx.GetString(TEXT("assetPath"));
    if (AssetPath.IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("Missing assetPath."));
        return true;
    }

    FString TagString = Ctx.GetString(TEXT("tag"));
    if (TagString.IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("Missing tag."));
        return true;
    }

    FGameplayTag Tag = GetOrRequestTag(TagString);
    if (!Tag.IsValid())
    {
        Ctx.SendError(TEXT("INVALID_TAG"), FString::Printf(TEXT("Invalid gameplay tag: %s"), *TagString));
        return true;
    }

    UObject* Asset = LoadObject<UObject>(nullptr, *AssetPath);
    if (!Asset)
    {
        Ctx.SendError(TEXT("NOT_FOUND"), FString::Printf(TEXT("Asset not found: %s"), *AssetPath));
        return true;
    }

    FString AssetType = TEXT("Unknown");
    bool bTagAdded = false;

    UBlueprint* Blueprint = Cast<UBlueprint>(Asset);
    if (Blueprint && Blueprint->GeneratedClass)
    {
        UObject* CDO = Blueprint->GeneratedClass->GetDefaultObject();

        if (UGameplayAbility* AbilityCDO = Cast<UGameplayAbility>(CDO))
        {
            PRAGMA_DISABLE_DEPRECATION_WARNINGS
            AbilityCDO->AbilityTags.AddTag(Tag);
            PRAGMA_ENABLE_DEPRECATION_WARNINGS
            AssetType = TEXT("GameplayAbility");
            bTagAdded = true;
            FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
            McpSafeAssetSave(Blueprint);
        }
        else if (UGameplayEffect* EffectCDO = Cast<UGameplayEffect>(CDO))
        {
            // Grant through the 5.3+ component model (same root cause as
            // set_effect_tags) so the GE actually grants the tag.
            GrantTagsOnEffect(EffectCDO, { Tag });
            AssetType = TEXT("GameplayEffect");
            bTagAdded = true;
            FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
            McpSafeAssetSave(Blueprint);
        }
        else if (UGameplayCueNotify_Static* CueStaticCDO = Cast<UGameplayCueNotify_Static>(CDO))
        {
            CueStaticCDO->GameplayCueTag = Tag;
            AssetType = TEXT("GameplayCueNotify_Static");
            bTagAdded = true;
            FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
            McpSafeAssetSave(Blueprint);
        }
        else if (AGameplayCueNotify_Actor* CueActorCDO = Cast<AGameplayCueNotify_Actor>(CDO))
        {
            CueActorCDO->GameplayCueTag = Tag;
            AssetType = TEXT("GameplayCueNotify_Actor");
            bTagAdded = true;
            FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
            McpSafeAssetSave(Blueprint);
        }
        // Actor blueprints (including ones whose SCS owns an AbilitySystemComponent)
        // are deliberately NOT handled: an ASC component template has no authorable
        // default-tag UPROPERTY to write the tag into. The former branch faked success
        // by adding a blank "OwnedGameplayTags" member variable nothing reads, dropping
        // the requested tag while reporting tagAdded:true. Such targets now fall through
        // to the UNSUPPORTED_TYPE rejection below.
    }

    if (!bTagAdded)
    {
        Ctx.SendError(TEXT("UNSUPPORTED_TYPE"),
            TEXT("Unsupported target for gas.add_tag_to_asset. Supported: GameplayAbility (ability tags), "
                 "GameplayEffect (granted tags), GameplayCueNotify_Static and GameplayCueNotify_Actor (cue tag). "
                 "An Actor owning an AbilitySystemComponent is not a valid target - an ASC template has no "
                 "authorable default-tag property; grant tags to the actor at runtime with a GameplayEffect "
                 "(gas.set_effect_tags) applied to its ASC."));
        return true;
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("assetPath"), AssetPath);
    Result->SetStringField(TEXT("tag"), TagString);
    Result->SetStringField(TEXT("assetType"), AssetType);
    Result->SetBoolField(TEXT("tagValid"), Tag.IsValid());
    Result->SetBoolField(TEXT("tagAdded"), bTagAdded);
    Ctx.SendSuccess(Result);
    return true;
#endif
}

// ---- gas.get_gas_info ----
REGISTER_RPC_HANDLER("gas.get_gas_info", "gas", "Get GAS information about an asset",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath", "path", "Path to the GAS asset")
    ))
{
#if !MCP_HAS_GAS
    Ctx.SendError(TEXT("GAS_NOT_AVAILABLE"), TEXT("GameplayAbilities plugin not enabled."));
    return true;
#else
    FString AssetPath = Ctx.GetString(TEXT("assetPath"));
    if (AssetPath.IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("Missing assetPath."));
        return true;
    }

    UObject* Asset = LoadObject<UObject>(nullptr, *AssetPath);
    if (!Asset)
    {
        Ctx.SendError(TEXT("NOT_FOUND"), FString::Printf(TEXT("Asset not found: %s"), *AssetPath));
        return true;
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("assetPath"), AssetPath);
    Result->SetStringField(TEXT("assetName"), Asset->GetName());
    Result->SetStringField(TEXT("class"), Asset->GetClass()->GetName());

    if (UBlueprint* Blueprint = Cast<UBlueprint>(Asset))
    {
        Result->SetStringField(TEXT("type"), TEXT("Blueprint"));
        if (Blueprint->GeneratedClass)
        {
            Result->SetStringField(TEXT("generatedClass"), Blueprint->GeneratedClass->GetName());

            UClass* ParentClass = Blueprint->ParentClass;
            if (ParentClass)
            {
                Result->SetStringField(TEXT("parentClass"), ParentClass->GetName());

                if (ParentClass->IsChildOf(UGameplayAbility::StaticClass()))
                {
                    Result->SetStringField(TEXT("gasType"), TEXT("GameplayAbility"));

                    UGameplayAbility* AbilityCDO = Cast<UGameplayAbility>(
                        Blueprint->GeneratedClass->GetDefaultObject());
                    if (AbilityCDO)
                    {
                        TEnumAsByte<EGameplayAbilityInstancingPolicy::Type> InstPolicy;
                        TEnumAsByte<EGameplayAbilityNetExecutionPolicy::Type> NetPolicy;

                        if (GetAbilityPropertyValue(AbilityCDO, FName(TEXT("InstancingPolicy")), InstPolicy))
                        {
                            Result->SetNumberField(TEXT("instancingPolicy"), static_cast<int32>(InstPolicy));
                        }
                        else
                        {
                            Result->SetNumberField(TEXT("instancingPolicy"), -1);
                        }

                        if (GetAbilityPropertyValue(AbilityCDO, FName(TEXT("NetExecutionPolicy")), NetPolicy))
                        {
                            Result->SetNumberField(TEXT("netExecutionPolicy"), static_cast<int32>(NetPolicy));
                        }
                        else
                        {
                            Result->SetNumberField(TEXT("netExecutionPolicy"), -1);
                        }

                        // Surface the wired cooldown / cost GE classes (written by
                        // gas.set_ability_cooldown / gas.set_ability_costs) and the
                        // ability tags (gas.set_ability_tags) so the canonical
                        // "confirm the cooldown and cost are wired" readback is
                        // satisfiable from this tool instead of an out-of-band
                        // property.get on the CDO. Empty string = unset.
                        Result->SetStringField(TEXT("cooldownEffect"),
                            ReadAbilityEffectClassPath(AbilityCDO, FName(TEXT("CooldownGameplayEffectClass"))));
                        Result->SetStringField(TEXT("costEffect"),
                            ReadAbilityEffectClassPath(AbilityCDO, FName(TEXT("CostGameplayEffectClass"))));
                        Result->SetArrayField(TEXT("abilityTags"),
                            EmitStringArray(CollectAbilityTags(AbilityCDO)));
                        // Activation-gating containers written by gas.set_ability_tags'
                        // activationBlockedTags / activationRequiredTags / activationOwnedTags
                        // params, so an "is this ability blocked while stunned?" readback is
                        // satisfiable here instead of via an out-of-band asset.dump.
                        Result->SetArrayField(TEXT("activationBlockedTags"),
                            EmitStringArray(CollectAbilityContainerTags(AbilityCDO, FName(TEXT("ActivationBlockedTags")))));
                        Result->SetArrayField(TEXT("activationRequiredTags"),
                            EmitStringArray(CollectAbilityContainerTags(AbilityCDO, FName(TEXT("ActivationRequiredTags")))));
                        Result->SetArrayField(TEXT("activationOwnedTags"),
                            EmitStringArray(CollectAbilityContainerTags(AbilityCDO, FName(TEXT("ActivationOwnedTags")))));
                    }
                }
                else if (ParentClass->IsChildOf(UGameplayEffect::StaticClass()))
                {
                    Result->SetStringField(TEXT("gasType"), TEXT("GameplayEffect"));

                    UGameplayEffect* EffectCDO = Cast<UGameplayEffect>(
                        Blueprint->GeneratedClass->GetDefaultObject());
                    if (EffectCDO)
                    {
                        Result->SetNumberField(TEXT("durationPolicy"),
                            static_cast<int32>(EffectCDO->DurationPolicy));
                        // Readable policy name so callers don't cross-reference the enum
                        // to decode the int (e.g. 2 -> "HasDuration").
                        Result->SetStringField(TEXT("durationPolicyName"),
                            JsonBuilders::EnumValueToString(EffectCDO->DurationPolicy));
                        // Surface the DurationMagnitude value (the seconds gas.set_effect_duration
                        // writes), not just the policy enum, so the readback can confirm the
                        // duration *is 8s*, not merely that it has-duration. Only meaningful for
                        // HasDuration and when the magnitude is a static ScalableFloat.
                        if (EffectCDO->DurationPolicy == EGameplayEffectDurationType::HasDuration)
                        {
                            float DurationMag = 0.0f;
                            if (EffectCDO->DurationMagnitude.GetStaticMagnitudeIfPossible(1.0f, DurationMag))
                            {
                                Result->SetNumberField(TEXT("durationMagnitude"), DurationMag);
                            }
                        }
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 7, 0)
                        PRAGMA_DISABLE_DEPRECATION_WARNINGS
#endif
                        Result->SetNumberField(TEXT("stackingType"),
                            static_cast<int32>(EffectCDO->StackingType));
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 7, 0)
                        PRAGMA_ENABLE_DEPRECATION_WARNINGS
#endif
                        Result->SetNumberField(TEXT("modifierCount"), EffectCDO->Modifiers.Num());
                        Result->SetNumberField(TEXT("cueCount"), EffectCDO->GameplayCues.Num());

                        // Surface each modifier's op / magnitude / bound attribute (written by
                        // gas.add_effect_modifier / set_modifier_magnitude / set_modifier_attribute)
                        // so the readback can confirm WHICH op and magnitude each modifier carries,
                        // not just modifierCount. modifierCount stays as an at-a-glance summary.
                        Result->SetArrayField(TEXT("modifiers"), CollectEffectModifiers(EffectCDO));

                        // Surface the wired execution calculations (written by
                        // gas.add_effect_execution_calculation) so the readback can
                        // confirm the execution calc is attached, not just modifiers.
                        Result->SetNumberField(TEXT("executionCount"), EffectCDO->Executions.Num());
                        TArray<TSharedPtr<FJsonValue>> ExecClassesJson;
                        for (const FGameplayEffectExecutionDefinition& ExecDef : EffectCDO->Executions)
                        {
                            if (ExecDef.CalculationClass)
                            {
                                ExecClassesJson.Add(MakeShared<FJsonValueString>(
                                    ExecDef.CalculationClass->GetName()));
                            }
                        }
                        Result->SetArrayField(TEXT("executionClasses"), ExecClassesJson);

                        // Surface the granted tags (written by gas.set_effect_tags)
                        // so the readback can confirm the configured tags, not just
                        // counts. Read from GetGrantedTags() (the component model) so
                        // the readback matches the live CDO and the cooldown validator.
                        Result->SetArrayField(TEXT("grantedTags"),
                            EmitStringArray(CollectGrantedTags(EffectCDO)));
                    }
                }
                else if (ParentClass->IsChildOf(UAttributeSet::StaticClass()))
                {
                    Result->SetStringField(TEXT("gasType"), TEXT("AttributeSet"));

                    // Enumerate the attributes (written by gas.add_attribute, with their
                    // default/base value from gas.set_attribute_base_value) so the readback can
                    // confirm an add_attribute run — names AND default values — instead of an
                    // asset.dump pivot. Each attribute is an FGameplayAttributeData FStructProperty
                    // on the generated class; its default lives in the BaseValue subproperty on the CDO.
                    Result->SetArrayField(TEXT("attributes"),
                        CollectAttributeSetAttributes(Blueprint->GeneratedClass));
                }
                else if (ParentClass->IsChildOf(UGameplayCueNotify_Static::StaticClass()))
                {
                    Result->SetStringField(TEXT("gasType"), TEXT("GameplayCueNotify_Static"));
                }
                else if (ParentClass->IsChildOf(AGameplayCueNotify_Actor::StaticClass()))
                {
                    Result->SetStringField(TEXT("gasType"), TEXT("GameplayCueNotify_Actor"));
                }
                else if (ParentClass->IsChildOf(UGameplayEffectExecutionCalculation::StaticClass()))
                {
                    // Surface the attribute captures (written by gas.set_execution_capture
                    // into RelevantAttributesToCapture) so the canonical "read back the
                    // captures on the exec calc" step is satisfiable from this tool instead
                    // of an out-of-band property.get on Default__<Exec>_C. Each entry is
                    // attribute / source / snapshot, read through the engine's public
                    // GetAttributeCaptureDefinitions() accessor. Branch on the exec-calc
                    // subclass specifically (not the UGameplayEffectCalculation base) so
                    // the gasType label stays accurate and MMC blueprints aren't mislabelled.
                    Result->SetStringField(TEXT("gasType"), TEXT("GameplayEffectExecutionCalculation"));
                    Result->SetArrayField(TEXT("captures"),
                        CollectExecutionCaptures(Blueprint->GeneratedClass));
                }
                else
                {
                    // Not one of the GAS *asset* parent classes. Detect an actor
                    // blueprint that OWNS an AbilitySystemComponent (the pawn the
                    // ASC/attributes/abilities are configured for) — otherwise such
                    // a blueprint falls through to bare metadata with no gasType and
                    // the GAS readback misleadingly looks empty. Uses the shared
                    // SCS-plus-CDO ASC detection helper (ForEachOwnedASC).
                    CollectAbilitySystemOwnerInfo(Blueprint, Result);
                }
            }
        }
    }

    Ctx.SendSuccess(Result);
    return true;
#endif
}

// ============================================================================
// 13.6 ABILITY SETS
// ============================================================================

// ---- gas.create_ability_set ----
REGISTER_RPC_HANDLER("gas.create_ability_set", "gas", "Create a data asset to hold granted abilities and effects",
    RPC_PARAMS(
        RPC_PARAM_OPT("setPath", "path", "Full path for the ability set asset"),
        RPC_PARAM_OPT("assetPath", "path", "Alternative to setPath"),
        RPC_PARAM_OPT("setName", "string", "Display name for the ability set")
    ))
{
#if !MCP_HAS_GAS
    Ctx.SendError(TEXT("GAS_NOT_AVAILABLE"), TEXT("GameplayAbilities plugin not enabled."));
    return true;
#else
    FString SetPath = Ctx.GetStringFirstOf({TEXT("setPath"), TEXT("assetPath")});
    if (SetPath.IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("Missing setPath or assetPath"));
        return true;
    }

    if (!IsValidMountPoint(SetPath))
    {
        // `TEXT("/Game") / P`, never `TEXT("/Game/") + P`. Concatenation MANUFACTURES a "//"
        // whenever P already carries a leading slash - "/NotAMount/X" became "/Game//NotAMount/X"
        // - and that byte sequence is what CreatePackage logs Fatal on. FString::operator/ routes
        // through PathAppend, which absorbs the duplicate separator. The IsValidLongPackageName
        // check below would refuse the "//" string, but composing it correctly is the fix; a
        // downstream check is not a licence to build a lethal string first.
        SetPath = FString(TEXT("/Game")) / SetPath;
    }

    // CHECKED HERE, above BOTH the LoadObject below and the CreatePackage under it.
    // CreatePackage (UObjectGlobals.cpp:1094-1096) logs at Fatal - a verbosity that is not
    // compiled out in any configuration - on a name containing "//", so an unvalidated caller
    // string does not fail the call, it ends the editor PROCESS and every unsaved package in it
    // (measured on B-foliage-add-type-name-with-slash-kills-the-editor). Two compositions here
    // reach it: a caller "//" survives untouched, and the mount-point fallback directly above
    // manufactures one from any unmounted rooted path ("/NotAMount/X" -> "/Game//NotAMount/X").
    // The LoadObject is the reason this cannot sit immediately above CreatePackage: on a path
    // with no '.' StaticLoadObjectInternal retries as "<path>.<shortname>", and ResolveName2
    // then calls CreatePackage on the package half itself (UObjectGlobals.cpp:1297-1311), so the
    // existence check is a second door to the same Fatal. Board:
    // B-createpackage-unvalidated-paths-plugin-wide.
    FText SetPathReason;
    if (!FPackageName::IsValidLongPackageName(SetPath, /*bIncludeReadOnlyRoots=*/true, &SetPathReason))
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"),
            FString::Printf(TEXT("'%s' is not a valid package path: %s"), *SetPath,
                *SetPathReason.ToString()));
        return true;
    }

    FString PackagePath, AssetName;
    int32 LastSlash;
    if (SetPath.FindLastChar('/', LastSlash))
    {
        PackagePath = SetPath.Left(LastSlash);
        AssetName = SetPath.RightChop(LastSlash + 1);
    }
    else
    {
        PackagePath = TEXT("/Game");
        AssetName = SetPath;
    }

    if (UObject* ExistingAsset = LoadObject<UObject>(nullptr, *SetPath))
    {
        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetStringField(TEXT("setPath"), SetPath);
        Result->SetStringField(TEXT("status"), TEXT("already_exists"));
        Ctx.SendSuccess(Result);
        return true;
    }

    FString PackageName = SetPath;
    UPackage* Package = CreatePackage(*PackageName);
    if (!Package)
    {
        Ctx.SendError(TEXT("PACKAGE_FAILED"), TEXT("Failed to create package"));
        return true;
    }

    UBlueprintFactory* Factory = NewObject<UBlueprintFactory>();
    Factory->ParentClass = UPrimaryDataAsset::StaticClass();

    UBlueprint* SetBlueprint = Cast<UBlueprint>(Factory->FactoryCreateNew(
        UBlueprint::StaticClass(),
        Package,
        *AssetName,
        RF_Public | RF_Standalone,
        nullptr,
        GWarn
    ));

    if (!SetBlueprint)
    {
        Ctx.SendError(TEXT("CREATION_FAILED"), TEXT("Failed to create ability set blueprint"));
        return true;
    }

    // GrantedAbilities array
    FEdGraphPinType AbilityArrayType;
    AbilityArrayType.PinCategory = UEdGraphSchema_K2::PC_SoftClass;
    AbilityArrayType.PinSubCategoryObject = UGameplayAbility::StaticClass();
    AbilityArrayType.ContainerType = EPinContainerType::Array;

    FBlueprintEditorUtils::AddMemberVariable(SetBlueprint, TEXT("GrantedAbilities"), AbilityArrayType);
    FBlueprintEditorUtils::SetBlueprintVariableCategory(SetBlueprint, TEXT("GrantedAbilities"), nullptr,
        FText::FromString(TEXT("Ability Set")));

    // GrantedEffects array
    FEdGraphPinType EffectArrayType;
    EffectArrayType.PinCategory = UEdGraphSchema_K2::PC_SoftClass;
    EffectArrayType.PinSubCategoryObject = UGameplayEffect::StaticClass();
    EffectArrayType.ContainerType = EPinContainerType::Array;

    FBlueprintEditorUtils::AddMemberVariable(SetBlueprint, TEXT("GrantedEffects"), EffectArrayType);
    FBlueprintEditorUtils::SetBlueprintVariableCategory(SetBlueprint, TEXT("GrantedEffects"), nullptr,
        FText::FromString(TEXT("Ability Set")));

    // GrantedTags
    FEdGraphPinType TagContainerType;
    TagContainerType.PinCategory = UEdGraphSchema_K2::PC_Struct;
    TagContainerType.PinSubCategoryObject = FGameplayTagContainer::StaticStruct();

    FBlueprintEditorUtils::AddMemberVariable(SetBlueprint, TEXT("GrantedTags"), TagContainerType);
    FBlueprintEditorUtils::SetBlueprintVariableCategory(SetBlueprint, TEXT("GrantedTags"), nullptr,
        FText::FromString(TEXT("Ability Set")));

    // SetDisplayName
    FEdGraphPinType StringType;
    StringType.PinCategory = UEdGraphSchema_K2::PC_String;
    FBlueprintEditorUtils::AddMemberVariable(SetBlueprint, TEXT("SetDisplayName"), StringType);

    FString SetName = Ctx.GetString(TEXT("setName"));
    if (SetName.IsEmpty())
    {
        SetName = AssetName;
    }

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(SetBlueprint);

    FAssetRegistryModule::AssetCreated(SetBlueprint);
    McpSafeAssetSave(SetBlueprint);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("setPath"), SetBlueprint->GetPathName());
    Result->SetStringField(TEXT("setName"), SetName);
    Result->SetStringField(TEXT("assetName"), AssetName);

    TArray<TSharedPtr<FJsonValue>> VariablesArray;
    VariablesArray.Add(MakeShared<FJsonValueString>(TEXT("GrantedAbilities")));
    VariablesArray.Add(MakeShared<FJsonValueString>(TEXT("GrantedEffects")));
    VariablesArray.Add(MakeShared<FJsonValueString>(TEXT("GrantedTags")));
    VariablesArray.Add(MakeShared<FJsonValueString>(TEXT("SetDisplayName")));
    Result->SetArrayField(TEXT("variables"), VariablesArray);

    Ctx.SendSuccess(Result);
    return true;
#endif
}

// ============================================================================
// 13.7 EXECUTION CALCULATIONS
// ============================================================================

// ---- gas.create_execution_calculation ----
REGISTER_RPC_HANDLER("gas.create_execution_calculation", "gas", "Create a GameplayEffectExecutionCalculation blueprint",
    RPC_PARAMS(
        RPC_PARAM_REQ("name", "string", "Name for the execution calculation blueprint"),
        RPC_PARAM_DEF("path", "path", "Content path for the asset", "/Game")
    ))
{
#if !MCP_HAS_GAS
    Ctx.SendError(TEXT("GAS_NOT_AVAILABLE"), TEXT("GameplayAbilities plugin not enabled."));
    return true;
#else
    FString Name = Ctx.GetString(TEXT("name"));
    FString Path = Ctx.GetString(TEXT("path"), TEXT("/Game"));

    if (Name.IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("Missing name."));
        return true;
    }

    FString Error;
    UBlueprint* Blueprint = CreateGASBlueprint(Path, Name, UGameplayEffectExecutionCalculation::StaticClass(), Error);
    if (!Blueprint)
    {
        Ctx.SendError(TEXT("CREATION_FAILED"), Error);
        return true;
    }

    // CapturedSourceAttributes
    FEdGraphPinType StructArrayType;
    StructArrayType.PinCategory = UEdGraphSchema_K2::PC_Struct;
    StructArrayType.PinSubCategoryObject = FGameplayAttribute::StaticStruct();
    StructArrayType.ContainerType = EPinContainerType::Array;

    FBlueprintEditorUtils::AddMemberVariable(Blueprint, TEXT("CapturedSourceAttributes"), StructArrayType);
    FBlueprintEditorUtils::SetBlueprintVariableCategory(Blueprint, TEXT("CapturedSourceAttributes"), nullptr,
        FText::FromString(TEXT("Execution Calculation")));

    FBlueprintEditorUtils::AddMemberVariable(Blueprint, TEXT("CapturedTargetAttributes"), StructArrayType);
    FBlueprintEditorUtils::SetBlueprintVariableCategory(Blueprint, TEXT("CapturedTargetAttributes"), nullptr,
        FText::FromString(TEXT("Execution Calculation")));

    // bRequiresPassedInTags
    FEdGraphPinType BoolPinType;
    BoolPinType.PinCategory = UEdGraphSchema_K2::PC_Boolean;
    FBlueprintEditorUtils::AddMemberVariable(Blueprint, TEXT("bRequiresPassedInTags"), BoolPinType);
    FBlueprintEditorUtils::SetBlueprintVariableCategory(Blueprint, TEXT("bRequiresPassedInTags"), nullptr,
        FText::FromString(TEXT("Execution Calculation")));

    // CalculationDescription
    FEdGraphPinType StringPinType;
    StringPinType.PinCategory = UEdGraphSchema_K2::PC_String;
    FBlueprintEditorUtils::AddMemberVariable(Blueprint, TEXT("CalculationDescription"), StringPinType);
    FBlueprintEditorUtils::SetBlueprintVariableCategory(Blueprint, TEXT("CalculationDescription"), nullptr,
        FText::FromString(TEXT("Execution Calculation")));

    // OutputModifierAttributes
    FBlueprintEditorUtils::AddMemberVariable(Blueprint, TEXT("OutputModifierAttributes"), StructArrayType);
    FBlueprintEditorUtils::SetBlueprintVariableCategory(Blueprint, TEXT("OutputModifierAttributes"), nullptr,
        FText::FromString(TEXT("Execution Calculation")));

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
    const BlueprintHandlerUtils::FBlueprintCompileDiagnostics CompileDiagnostics =
        BlueprintHandlerUtils::CompileBlueprintWithDiagnostics(Blueprint);
    McpSafeAssetSave(Blueprint);

    FString ActualName = Blueprint->GetName();
    FString ActualPath = Path / ActualName;

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("assetPath"), ActualPath);
    Result->SetStringField(TEXT("name"), ActualName);
    Result->SetStringField(TEXT("parentClass"), TEXT("GameplayEffectExecutionCalculation"));

    TArray<TSharedPtr<FJsonValue>> VariablesArray;
    VariablesArray.Add(MakeShared<FJsonValueString>(TEXT("CapturedSourceAttributes")));
    VariablesArray.Add(MakeShared<FJsonValueString>(TEXT("CapturedTargetAttributes")));
    VariablesArray.Add(MakeShared<FJsonValueString>(TEXT("bRequiresPassedInTags")));
    VariablesArray.Add(MakeShared<FJsonValueString>(TEXT("CalculationDescription")));
    VariablesArray.Add(MakeShared<FJsonValueString>(TEXT("OutputModifierAttributes")));
    Result->SetArrayField(TEXT("variablesAdded"), VariablesArray);

    Result->SetStringField(TEXT("note"), TEXT("Override Execute_Implementation in Blueprint to implement custom calculation logic. Use CapturedSourceAttributes and CapturedTargetAttributes to define which attributes to capture."));
    BlueprintHandlerUtils::AddCompileDiagnosticsToJson(CompileDiagnostics, Result);

    Ctx.SendSuccess(Result);
    return true;
#endif
}

// ---- gas.set_effect_period ----
REGISTER_RPC_HANDLER("gas.set_effect_period", "gas", "Configure periodic settings on a GameplayEffect (Period, ExecuteOnApplication, InhibitionPolicy)",
    RPC_PARAMS(
        RPC_PARAM_REQ("blueprintPath", "path", "Path to the effect blueprint"),
        RPC_PARAM_REQ("periodSeconds", "number", "Period in seconds (writes Period.Value)"),
        RPC_PARAM_OPT("periodicMagnitude", "number", "Optional scalable-float magnitude assigned to Modifiers[0] per-tick"),
        RPC_PARAM_DEF("executeOnApplication", "bool", "Whether to execute on application (bExecutePeriodicEffectOnApplication)", "true"),
        RPC_PARAM_DEF("inhibitionPolicy", "string", "never_reset | reset_period | execute_and_reset_period", "never_reset")
    ))
{
#if !MCP_HAS_GAS
    Ctx.SendError(TEXT("GAS_NOT_AVAILABLE"), TEXT("GameplayAbilities plugin not enabled."));
    return true;
#else
    FString BlueprintPath;
    if (!Ctx.RequireString(TEXT("blueprintPath"), BlueprintPath)) return true;

    UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *BlueprintPath);
    if (!Blueprint || !Blueprint->GeneratedClass)
    {
        Ctx.SendError(TEXT("NOT_FOUND"), FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintPath));
        return true;
    }

    UGameplayEffect* EffectCDO = Cast<UGameplayEffect>(Blueprint->GeneratedClass->GetDefaultObject());
    if (!EffectCDO)
    {
        Ctx.SendError(TEXT("NOT_A_GAMEPLAY_EFFECT"), TEXT("Blueprint generated class is not a UGameplayEffect"));
        return true;
    }

    if (EffectCDO->DurationPolicy == EGameplayEffectDurationType::Instant)
    {
        Ctx.SendError(TEXT("NOT_PERIODIC"), TEXT("Period is meaningless on Instant effects. Set durationType to has_duration or infinite first."));
        return true;
    }

    const float PeriodSeconds = static_cast<float>(Ctx.GetNumber(TEXT("periodSeconds"), 0.0));
    const bool bExecuteOnApp = Ctx.GetBool(TEXT("executeOnApplication"), true);
    const FString InhibitionPolicy = Ctx.GetString(TEXT("inhibitionPolicy"), TEXT("never_reset"));

    static const struct { const TCHAR* Name; EGameplayEffectPeriodInhibitionRemovedPolicy Value; } PolicyMap[] = {
        { TEXT("never_reset"),              EGameplayEffectPeriodInhibitionRemovedPolicy::NeverReset },
        { TEXT("reset_period"),             EGameplayEffectPeriodInhibitionRemovedPolicy::ResetPeriod },
        { TEXT("execute_and_reset_period"), EGameplayEffectPeriodInhibitionRemovedPolicy::ExecuteAndResetPeriod },
    };
    EGameplayEffectPeriodInhibitionRemovedPolicy ResolvedPolicy = EGameplayEffectPeriodInhibitionRemovedPolicy::NeverReset;
    bool bFoundPolicy = false;
    for (const auto& Entry : PolicyMap)
    {
        if (InhibitionPolicy.Equals(Entry.Name, ESearchCase::IgnoreCase))
        {
            ResolvedPolicy = Entry.Value;
            bFoundPolicy = true;
            break;
        }
    }
    if (!bFoundPolicy)
    {
        Ctx.SendError(TEXT("INVALID_INHIBITION_POLICY"), FString::Printf(TEXT("Unknown inhibitionPolicy '%s'. Expected: never_reset | reset_period | execute_and_reset_period"), *InhibitionPolicy));
        return true;
    }

    EffectCDO->Period = FScalableFloat(PeriodSeconds);
    EffectCDO->bExecutePeriodicEffectOnApplication = bExecuteOnApp;
    EffectCDO->PeriodicInhibitionPolicy = ResolvedPolicy;

    // Optional periodic magnitude on Modifiers[0]
    const TSharedPtr<FJsonValue> PeriodicMagValue = Ctx.GetRawPayload().IsValid()
        ? Ctx.GetRawPayload()->TryGetField(TEXT("periodicMagnitude"))
        : nullptr;
    if (PeriodicMagValue.IsValid() && PeriodicMagValue->Type == EJson::Number && EffectCDO->Modifiers.Num() > 0)
    {
        const float PeriodicMag = static_cast<float>(PeriodicMagValue->AsNumber());
        EffectCDO->Modifiers[0].ModifierMagnitude = FGameplayEffectModifierMagnitude(FScalableFloat(PeriodicMag));
    }

    FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetNumberField(TEXT("periodSeconds"), PeriodSeconds);
    Result->SetBoolField(TEXT("executeOnApplication"), bExecuteOnApp);
    Result->SetStringField(TEXT("inhibitionPolicy"), InhibitionPolicy);
    Ctx.SendSuccess(Result);
    return true;
#endif
}

// ---- gas.set_ability_input ----
REGISTER_RPC_HANDLER("gas.set_ability_input", "gas", "Bind an input ID (and optional InputAction soft-ref) on a GameplayAbility blueprint CDO",
    RPC_PARAMS(
        RPC_PARAM_REQ("blueprintPath", "path", "Path to the ability blueprint"),
        RPC_PARAM_OPT("inputIDEnumValue", "string", "Name of the enum entry to resolve (requires enumPath)"),
        RPC_PARAM_OPT("inputIDValue", "number", "Explicit int value when no enum context is provided"),
        RPC_PARAM_OPT("inputActionPath", "path", "Soft-ref path to a UInputAction asset"),
        RPC_PARAM_DEF("bindOn", "string", "cdo | ability_set (ability_set not yet supported)", "cdo"),
        RPC_PARAM_OPT("enumPath", "path", "Object path of the UEnum holding the input ID values"),
        RPC_PARAM_DEF("propertyName", "string", "Name of an int/byte property that must already exist on the ability class (create_gameplay_ability does not add one; add it via blueprint.add_variable first). Absent -> PROPERTY_NOT_FOUND; present but not int/byte -> PROPERTY_NOT_INT", "AbilityInputID"),
        RPC_PARAM_OPT("abilitySetPath", "path", "Ability set path (only honored when bindOn=ability_set)")
    ))
{
#if !MCP_HAS_GAS
    Ctx.SendError(TEXT("GAS_NOT_AVAILABLE"), TEXT("GameplayAbilities plugin not enabled."));
    return true;
#else
    FString BlueprintPath;
    if (!Ctx.RequireString(TEXT("blueprintPath"), BlueprintPath)) return true;

    const FString BindOn = Ctx.GetString(TEXT("bindOn"), TEXT("cdo"));
    if (BindOn == TEXT("ability_set"))
    {
        Ctx.SendError(TEXT("NOT_IMPLEMENTED"), TEXT("ability_set mode requires create_ability_set to emit an InputAbilities variable; not yet supported"));
        return true;
    }

    UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *BlueprintPath);
    if (!Blueprint || !Blueprint->GeneratedClass)
    {
        Ctx.SendError(TEXT("NOT_FOUND"), FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintPath));
        return true;
    }

    UGameplayAbility* Ability = Cast<UGameplayAbility>(Blueprint->GeneratedClass->GetDefaultObject());
    if (!Ability)
    {
        Ctx.SendError(TEXT("NOT_A_GAMEPLAY_ABILITY"), TEXT("Blueprint generated class is not a UGameplayAbility"));
        return true;
    }

    const FString InputIDEnumValue = Ctx.GetString(TEXT("inputIDEnumValue"));
    const FString EnumPath = Ctx.GetString(TEXT("enumPath"));
    const FString InputActionPath = Ctx.GetString(TEXT("inputActionPath"));
    const FString PropertyName = Ctx.GetString(TEXT("propertyName"), TEXT("AbilityInputID"));

    // Resolve input ID value
    int64 ResolvedInputID = 0;
    bool bHaveInputID = false;
    if (!InputIDEnumValue.IsEmpty() && !EnumPath.IsEmpty())
    {
        UEnum* InputEnum = LoadObject<UEnum>(nullptr, *EnumPath);
        if (!InputEnum)
        {
            Ctx.SendError(TEXT("ENUM_NOT_RESOLVED"), FString::Printf(TEXT("Could not load UEnum at '%s'"), *EnumPath));
            return true;
        }
        const int64 EnumVal = InputEnum->GetValueByNameString(InputIDEnumValue);
        if (EnumVal == INDEX_NONE)
        {
            Ctx.SendError(TEXT("ENUM_VALUE_NOT_FOUND"), FString::Printf(TEXT("Enum '%s' has no entry '%s'"), *EnumPath, *InputIDEnumValue));
            return true;
        }
        ResolvedInputID = EnumVal;
        bHaveInputID = true;
    }
    else
    {
        const TSharedPtr<FJsonValue> InputIDValueRaw = Ctx.GetRawPayload().IsValid()
            ? Ctx.GetRawPayload()->TryGetField(TEXT("inputIDValue"))
            : nullptr;
        if (InputIDValueRaw.IsValid() && InputIDValueRaw->Type == EJson::Number)
        {
            ResolvedInputID = static_cast<int64>(InputIDValueRaw->AsNumber());
            bHaveInputID = true;
        }
    }

    if (!bHaveInputID && InputActionPath.IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_PARAMS"), TEXT("Provide either inputIDEnumValue+enumPath, inputIDValue, or inputActionPath"));
        return true;
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();

    if (bHaveInputID)
    {
        FProperty* Prop = Ability->GetClass()->FindPropertyByName(FName(*PropertyName));
        if (!Prop)
        {
            Ctx.SendError(TEXT("PROPERTY_NOT_FOUND"), FString::Printf(TEXT("Property '%s' not found on ability class"), *PropertyName));
            return true;
        }
        if (FIntProperty* IntProp = CastField<FIntProperty>(Prop))
        {
            IntProp->SetPropertyValue_InContainer(Ability, static_cast<int32>(ResolvedInputID));
        }
        else if (FByteProperty* ByteProp = CastField<FByteProperty>(Prop))
        {
            ByteProp->SetPropertyValue_InContainer(Ability, static_cast<uint8>(ResolvedInputID));
        }
        else
        {
            Ctx.SendError(TEXT("PROPERTY_NOT_INT"), FString::Printf(TEXT("Property '%s' is not an int or byte property"), *PropertyName));
            return true;
        }
        Result->SetNumberField(TEXT("appliedInputID"), static_cast<double>(ResolvedInputID));
    }

    if (!InputActionPath.IsEmpty())
    {
        // Find a property whose type is soft-ref or hard-ref to UInputAction
        FProperty* InputActionProp = nullptr;
        for (TFieldIterator<FProperty> It(Ability->GetClass()); It; ++It)
        {
            FProperty* Candidate = *It;
            if (FSoftObjectProperty* SoftProp = CastField<FSoftObjectProperty>(Candidate))
            {
                if (SoftProp->PropertyClass && SoftProp->PropertyClass->GetFName() == TEXT("InputAction"))
                {
                    InputActionProp = Candidate;
                    break;
                }
            }
            else if (FObjectProperty* ObjProp = CastField<FObjectProperty>(Candidate))
            {
                if (ObjProp->PropertyClass && ObjProp->PropertyClass->GetFName() == TEXT("InputAction"))
                {
                    InputActionProp = Candidate;
                    break;
                }
            }
        }
        if (!InputActionProp)
        {
            Ctx.SendError(TEXT("INPUT_ACTION_PROPERTY_NOT_FOUND"), TEXT("Ability class has no UInputAction soft-ref or hard-ref property to bind to"));
            return true;
        }
        if (FSoftObjectProperty* SoftProp = CastField<FSoftObjectProperty>(InputActionProp))
        {
            FSoftObjectPath SoftPath(InputActionPath);
            FSoftObjectPtr SoftPtr(SoftPath);
            SoftProp->SetPropertyValue_InContainer(Ability, SoftPtr);
        }
        else if (FObjectProperty* ObjProp = CastField<FObjectProperty>(InputActionProp))
        {
            UObject* LoadedAction = LoadObject<UObject>(nullptr, *InputActionPath);
            ObjProp->SetObjectPropertyValue_InContainer(Ability, LoadedAction);
        }
        Result->SetStringField(TEXT("appliedInputAction"), InputActionPath);
    }

    FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
    Ctx.SendSuccess(Result);
    return true;
#endif
}

// ---- gas.set_modifier_magnitude_setbycaller ----
REGISTER_RPC_HANDLER("gas.set_modifier_magnitude_setbycaller", "gas", "Convert a modifier magnitude to SetByCaller (DataTag and/or DataName)",
    RPC_PARAMS(
        RPC_PARAM_REQ("blueprintPath", "path", "Path to the effect blueprint"),
        RPC_PARAM_DEF("modifierIndex", "integer", "Index of the modifier to update", "0"),
        RPC_PARAM_OPT("dataTag", "string", "FGameplayTag for SetByCallerMagnitude.DataTag"),
        RPC_PARAM_OPT("dataName", "string", "FName for SetByCallerMagnitude.DataName (legacy fallback)")
    ))
{
#if !MCP_HAS_GAS
    Ctx.SendError(TEXT("GAS_NOT_AVAILABLE"), TEXT("GameplayAbilities plugin not enabled."));
    return true;
#else
    FString BlueprintPath;
    if (!Ctx.RequireString(TEXT("blueprintPath"), BlueprintPath)) return true;

    UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *BlueprintPath);
    if (!Blueprint || !Blueprint->GeneratedClass)
    {
        Ctx.SendError(TEXT("NOT_FOUND"), FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintPath));
        return true;
    }

    UGameplayEffect* EffectCDO = Cast<UGameplayEffect>(Blueprint->GeneratedClass->GetDefaultObject());
    if (!EffectCDO)
    {
        Ctx.SendError(TEXT("NOT_A_GAMEPLAY_EFFECT"), TEXT("Blueprint generated class is not a UGameplayEffect"));
        return true;
    }

    const int32 ModifierIndex = Ctx.GetInt(TEXT("modifierIndex"), 0);
    if (ModifierIndex < 0 || ModifierIndex >= EffectCDO->Modifiers.Num())
    {
        Ctx.SendError(TEXT("INVALID_INDEX"), TEXT("Modifier index out of range"));
        return true;
    }

    const FString DataTag = Ctx.GetString(TEXT("dataTag"));
    const FString DataName = Ctx.GetString(TEXT("dataName"));
    if (DataTag.IsEmpty() && DataName.IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_PARAMS"), TEXT("Provide at least one of dataTag or dataName"));
        return true;
    }

    FSetByCallerFloat SBC;
    if (!DataTag.IsEmpty())
    {
        SBC.DataTag = FGameplayTag::RequestGameplayTag(FName(*DataTag), false);
    }
    if (!DataName.IsEmpty())
    {
        SBC.DataName = FName(*DataName);
    }

    EffectCDO->Modifiers[ModifierIndex].ModifierMagnitude = FGameplayEffectModifierMagnitude(SBC);

    FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetNumberField(TEXT("modifierIndex"), ModifierIndex);
    Result->SetStringField(TEXT("dataTag"), DataTag);
    Result->SetStringField(TEXT("dataName"), DataName);
    Ctx.SendSuccess(Result);
    return true;
#endif
}

// ---- gas.set_execution_capture ----
REGISTER_RPC_HANDLER("gas.set_execution_capture", "gas", "Populate RelevantAttributesToCapture on a GameplayEffectExecutionCalculation blueprint CDO",
    RPC_PARAMS(
        RPC_PARAM_REQ("executionClassPath", "path", "Path to the execution-calculation blueprint"),
        RPC_PARAM_REQ("captures", "array", "Array of {attribute, source, snapshot} entries; 'attribute' must be 'AttributeSetClassPath_C.AttrName' naming a compiled property — see the gas wiki page")
    ))
{
#if !MCP_HAS_GAS
    Ctx.SendError(TEXT("GAS_NOT_AVAILABLE"), TEXT("GameplayAbilities plugin not enabled."));
    return true;
#else
    FString ExecPath;
    if (!Ctx.RequireString(TEXT("executionClassPath"), ExecPath)) return true;

    UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *ExecPath);
    if (!Blueprint || !Blueprint->GeneratedClass)
    {
        Ctx.SendError(TEXT("NOT_FOUND"), FString::Printf(TEXT("Blueprint not found: %s"), *ExecPath));
        return true;
    }

    UGameplayEffectCalculation* CalcCDO = Cast<UGameplayEffectCalculation>(Blueprint->GeneratedClass->GetDefaultObject());
    if (!CalcCDO)
    {
        Ctx.SendError(TEXT("NOT_AN_EFFECT_CALCULATION"), TEXT("Blueprint generated class is not a UGameplayEffectCalculation"));
        return true;
    }

    const TArray<TSharedPtr<FJsonValue>>* CapturesArray = nullptr;
    if (!Ctx.GetRawPayload().IsValid() || !Ctx.GetRawPayload()->TryGetArrayField(TEXT("captures"), CapturesArray) || !CapturesArray)
    {
        Ctx.SendError(TEXT("INVALID_PARAMS"), TEXT("Missing or invalid 'captures' array"));
        return true;
    }

    // Locate RelevantAttributesToCapture (protected field on UGameplayEffectCalculation) via reflection.
    FArrayProperty* ArrProp = CastField<FArrayProperty>(CalcCDO->GetClass()->FindPropertyByName(TEXT("RelevantAttributesToCapture")));
    if (!ArrProp)
    {
        Ctx.SendError(TEXT("CAPTURE_ARRAY_NOT_FOUND"), TEXT("Could not locate RelevantAttributesToCapture property on calculation class"));
        return true;
    }
    FStructProperty* StructProp = CastField<FStructProperty>(ArrProp->Inner);
    if (!StructProp || !StructProp->Struct)
    {
        Ctx.SendError(TEXT("CAPTURE_ARRAY_NOT_FOUND"), TEXT("RelevantAttributesToCapture inner element is not a struct property"));
        return true;
    }

    FScriptArrayHelper Helper(ArrProp, ArrProp->ContainerPtrToValuePtr<void>(CalcCDO));

    int32 CaptureCount = 0;
    for (const TSharedPtr<FJsonValue>& Value : *CapturesArray)
    {
        const TSharedPtr<FJsonObject>* Entry = nullptr;
        if (!Value.IsValid() || !Value->TryGetObject(Entry) || !Entry || !Entry->IsValid())
        {
            continue;
        }

        FString AttrSpec;
        if (!(*Entry)->TryGetStringField(TEXT("attribute"), AttrSpec))
        {
            Ctx.SendError(TEXT("INVALID_PARAMS"), TEXT("Each capture entry requires 'attribute'"));
            return true;
        }

        // Resolve via the shared "AttributeSetClassPath.AttrName" resolver.
        FGameplayAttribute Attr;
        FString AttrErrCode, AttrErrMsg;
        if (!ResolveGameplayAttributeFromSpec(AttrSpec, Attr, AttrErrCode, AttrErrMsg))
        {
            Ctx.SendError(AttrErrCode, AttrErrMsg);
            return true;
        }

        FString SourceStr;
        (*Entry)->TryGetStringField(TEXT("source"), SourceStr);
        EGameplayEffectAttributeCaptureSource SourceEnum = EGameplayEffectAttributeCaptureSource::Source;
        if (SourceStr.Equals(TEXT("target"), ESearchCase::IgnoreCase))
        {
            SourceEnum = EGameplayEffectAttributeCaptureSource::Target;
        }

        bool bSnapshot = false;
        (*Entry)->TryGetBoolField(TEXT("snapshot"), bSnapshot);

        FGameplayEffectAttributeCaptureDefinition Capture(Attr, SourceEnum, bSnapshot);

        Helper.AddValue();
        void* Slot = Helper.GetRawPtr(Helper.Num() - 1);
        StructProp->Struct->CopyScriptStruct(Slot, &Capture);
        ++CaptureCount;
    }

    FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetNumberField(TEXT("captureCount"), CaptureCount);
    Ctx.SendSuccess(Result);
    return true;
#endif
}
