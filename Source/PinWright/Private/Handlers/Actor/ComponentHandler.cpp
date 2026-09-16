// Copyright (c) 2026 Alexander Penkin. MIT License.

// ComponentHandler.cpp - Migrated from PinWright_ControlHandlers.cpp
// Handles actor.add_component, actor.set_component_properties, actor.get_components,
// actor.remove_component, actor.get_component_property

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ParamSpec.h"
#include "Handlers/ParamAliasUtils.h"
#include "Handlers/Actor/ActorNameParamUtils.h"
#include "PinWrightGlobals.h"
#include "PinWrightHelpers.h"
#include "PinWrightSubsystem.h"
#include "Utils/ActorUtils.h"
#include "Utils/AutoActivateDisclosure.h"
#include "Utils/BodyInstanceCollisionPropertyWrite.h"
#include "Utils/ComponentAssetPropertyWrite.h"
#include "Utils/ComponentReadFilter.h"
#include "Utils/PropertyChangeNotify.h"
#include "Utils/PropertyUtils.h"
#include "Utils/PropertyInspection.h"
#include "Utils/ShapeExtentPropertyWrite.h"
#include "Utils/AssetUtils.h"
#include "Dom/JsonObject.h"
#include "Compat/JsonKeyCompat.h"

#include "Editor.h"
#if __has_include("Subsystems/EditorActorSubsystem.h")
#include "Subsystems/EditorActorSubsystem.h"
#elif __has_include("EditorActorSubsystem.h")
#include "EditorActorSubsystem.h"
#endif
#include "Components/ActorComponent.h"
#include "Components/SceneComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/LightComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/Blueprint.h"
#include "GameFramework/Actor.h"

namespace {

// Neither verb used to say anything about a bAutoActivate:false write beyond echoing the
// property name, and that write outlives the session - see
// Utils/AutoActivateDisclosure.h for why, and for the text both share with property.set.
//
// Only the "did this request name the flag" half lives here, because only this verb knows
// its own request shape. The entry goes on the response's existing `warnings` array - the
// plugin's one shape for an outcome the `applied` list cannot carry (docs/rpc-design.md) -
// and, like every other entry there, only appears when there is something to say.
void DiscloseAutoActivateDisabled(const UActorComponent *Component,
                                  const TSharedPtr<FJsonObject> &Properties,
                                  TArray<FString> &OutWarnings) {
  if (!Properties.IsValid())
    return;

  bool bNamed = false;
  for (const auto &Pair : Properties->Values) {
    if (EARGCompat::JsonKeyToString(Pair.Key).Equals(TEXT("bAutoActivate"),
                                                     ESearchCase::IgnoreCase)) {
      bNamed = true;
      break;
    }
  }
  if (!bNamed)
    return;

  const FString Disclosure = PinWright::MakeAutoActivateDisabledDisclosure(Component);
  if (!Disclosure.IsEmpty())
    OutWarnings.Add(Disclosure);
}

} // namespace

// ---- actor.add_component ----
REGISTER_RPC_HANDLER("actor.add_component", "actor", "Add an instance-level component to a placed actor (level instance, not the BP class). For Blueprint class templates, use blueprint.scs.add_component instead. Static mesh components auto-load meshPath; lights are forced to Movable mobility.",
    RPC_PARAMS(
        RPC_PARAM_REQ("actorName", "string", "Display label or name of the target actor in the level."),
        RPC_PARAM_REQ("componentType", "classref", "UClass name (e.g. 'PointLightComponent') or full class path (e.g. '/Script/Engine.PointLightComponent'); must derive from UActorComponent."),
        RPC_PARAM_OPT("componentName", "string", "Identifier for the new component; auto-generated as <ClassName>_<rand> when omitted."),
        RPC_PARAM_OPT("meshPath", "path", "Asset path of a static mesh; only used when componentType is StaticMeshComponent."),
        RPC_PARAM_OPT("properties", "object", "Map of UPROPERTY names to JSON values applied after construction; failures returned in 'warnings' array, as is a write that leaves bAutoActivate false.")
    ))
{
  auto* Subsystem = Ctx.GetSubsystem();
  const auto& Payload = Ctx.GetRawPayload();

  FString TargetName = Ctx.GetString(TEXT("actorName"));
  if (TargetName.IsEmpty()) {
    Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("actorName required"));
    return true;
  }

  FString ComponentType = Ctx.GetString(TEXT("componentType"));
  if (ComponentType.IsEmpty()) {
    Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("componentType required"));
    return true;
  }

  FString ComponentName = Ctx.GetString(TEXT("componentName"));

  // Resolve with an explicit ambiguity verdict: a label matching several actors must not
  // be reported as "not found", and must never silently pick one.
  AActor *Found = nullptr;
  if (!ActorNameParamUtils::ResolveActorOrSendError(Ctx, nullptr, TargetName, Found)) {
    return true;
  }

  UClass *ComponentClass = ResolveClassByName(ComponentType);
  if (!ComponentClass ||
      !ComponentClass->IsChildOf(UActorComponent::StaticClass())) {
    Ctx.SendError(TEXT("CLASS_NOT_FOUND"), TEXT("Component class not found"));
    return true;
  }

  if (ComponentName.TrimStartAndEnd().IsEmpty())
    ComponentName = FString::Printf(TEXT("%s_%d"), *ComponentClass->GetName(),
                                    FMath::Rand());

  FName DesiredName = FName(*ComponentName);
  UActorComponent *NewComponent = NewObject<UActorComponent>(
      Found, ComponentClass, DesiredName, RF_Transactional);
  if (!NewComponent) {
    Ctx.SendError(TEXT("CREATE_COMPONENT_FAILED"), TEXT("Failed to create component"));
    return true;
  }

  Found->Modify();
  NewComponent->SetFlags(RF_Transactional);
  Found->AddInstanceComponent(NewComponent);
  NewComponent->OnComponentCreated();

  if (USceneComponent *SceneComp = Cast<USceneComponent>(NewComponent)) {
    if (Found->GetRootComponent() && !SceneComp->GetAttachParent()) {
      SceneComp->SetupAttachment(Found->GetRootComponent());
    }
  }

  // Force lights to be movable
  if (NewComponent->IsA(ULightComponent::StaticClass())) {
    if (USceneComponent *SC = Cast<USceneComponent>(NewComponent)) {
      SC->SetMobility(EComponentMobility::Movable);
    }
  }

  // Special handling for StaticMeshComponent meshPath convenience
  if (UStaticMeshComponent *SMC = Cast<UStaticMeshComponent>(NewComponent)) {
    FString MeshPath = Ctx.GetString(TEXT("meshPath"));
    if (!MeshPath.IsEmpty()) {
      if (UObject *LoadedMesh = ResolveAsset(MeshPath, /*bLoadObject=*/true).Object) {
        if (UStaticMesh *Mesh = Cast<UStaticMesh>(LoadedMesh)) {
          SMC->SetStaticMesh(Mesh);
        }
      }
    }
  }

  TArray<FString> AppliedProperties;
  TArray<FString> PropertyWarnings;
  const TSharedPtr<FJsonObject> *PropertiesPtr = nullptr;
  if (Payload->TryGetObjectField(TEXT("properties"), PropertiesPtr) &&
      PropertiesPtr && (*PropertiesPtr).IsValid()) {
    for (const auto &Pair : (*PropertiesPtr)->Values) {
      FProperty *Property = ComponentClass->FindPropertyByName(*Pair.Key);
      if (!Property) {
        PropertyWarnings.Add(
            FString::Printf(TEXT("Property not found: %s"), *Pair.Key));
        continue;
      }
      FString ApplyError;
      // Shadow-copy properties (StaticMesh, SkinnedAsset) must go through the
      // engine's typed setter or the component's cached asset state diverges from
      // the serialized property - see Utils/ComponentAssetPropertyWrite.h.
      const PinWright::EComponentAssetWrite AssetWrite =
          PinWright::ApplyComponentAssetProperty(NewComponent, Property, Pair.Value,
                                                 ApplyError);
      if (AssetWrite == PinWright::EComponentAssetWrite::Applied)
        AppliedProperties.Add(EARGCompat::JsonKeyToString(Pair.Key));
      else if (AssetWrite == PinWright::EComponentAssetWrite::Failed)
        PropertyWarnings.Add(FString::Printf(TEXT("Failed to set %s: %s"),
                                             *Pair.Key, *ApplyError));
      else if (ApplyJsonValueToProperty(NewComponent, Property, Pair.Value,
                                        ApplyError))
        AppliedProperties.Add(EARGCompat::JsonKeyToString(Pair.Key));
      else
        PropertyWarnings.Add(FString::Printf(TEXT("Failed to set %s: %s"),
                                             *Pair.Key, *ApplyError));
    }
  }

  NewComponent->RegisterComponent();
  if (USceneComponent *SceneComp = Cast<USceneComponent>(NewComponent))
    SceneComp->UpdateComponentToWorld();
  NewComponent->MarkPackageDirty();
  Found->MarkPackageDirty();

  // Read after RegisterComponent: registration is what would have started the component
  // outside a game world, so this is the point at which "it will never run" is true.
  if (PropertiesPtr && (*PropertiesPtr).IsValid())
    DiscloseAutoActivateDisabled(NewComponent, *PropertiesPtr, PropertyWarnings);

  TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
  Resp->SetBoolField(TEXT("success"), true);
  Resp->SetStringField(TEXT("componentName"), NewComponent->GetName());
  Resp->SetStringField(TEXT("componentPath"), NewComponent->GetPathName());
  Resp->SetStringField(TEXT("componentClass"), ComponentClass->GetPathName());
  if (AppliedProperties.Num() > 0) {
    TArray<TSharedPtr<FJsonValue>> PropsArray;
    for (const FString &PropName : AppliedProperties)
      PropsArray.Add(MakeShared<FJsonValueString>(PropName));
    Resp->SetArrayField(TEXT("appliedProperties"), PropsArray);
  }
  if (PropertyWarnings.Num() > 0) {
    TArray<TSharedPtr<FJsonValue>> WarnArray;
    for (const FString &Warning : PropertyWarnings)
      WarnArray.Add(MakeShared<FJsonValueString>(Warning));
    Resp->SetArrayField(TEXT("warnings"), WarnArray);
  }
  UE_LOG(LogPinWrightSubsystem, Display,
         TEXT("actor.add_component: Added component '%s' to '%s'"),
         *NewComponent->GetName(), *Found->GetActorLabel());
  Ctx.SendSuccess(Resp);
  return true;
}

// ---- actor.set_component_properties ----
REGISTER_RPC_HANDLER("actor.set_component_properties", "actor", "Set one or more UPROPERTY values on a named component of a placed actor. Mobility is applied first (so subsequent edits succeed); SimulatePhysics has dedicated path. Per-property failures returned in 'warnings', as is a write that leaves bAutoActivate false (a level override that outlives the session).",
    RPC_PARAMS(
        RPC_PARAM_REQ("actorName", "string", "Display label or name of the actor." ACTORNAME_COLLISION_STEER),
        RPC_PARAM_REQ("componentName", "string", "Component identifier on the actor; matched case-insensitively."),
        RPC_PARAM_REQ("properties", "object", "Map of UPROPERTY names to JSON values; supports Mobility (string enum or int) and SimulatePhysics/bSimulatePhysics with special handling.")
    ))
{
  auto* Subsystem = Ctx.GetSubsystem();
  const auto& Payload = Ctx.GetRawPayload();

  FString TargetName = Ctx.GetString(TEXT("actorName"));
  if (TargetName.IsEmpty()) {
    Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("actorName required"));
    return true;
  }

  FString ComponentName = Ctx.GetString(TEXT("componentName"));
  if (ComponentName.IsEmpty()) {
    Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("componentName required"));
    return true;
  }

  const TSharedPtr<FJsonObject> *PropertiesPtr = nullptr;
  if (!(Payload->TryGetObjectField(TEXT("properties"), PropertiesPtr) &&
        PropertiesPtr && PropertiesPtr->IsValid())) {
    Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("properties object required"));
    return true;
  }

  // Resolve with an explicit ambiguity verdict: a label matching several actors must not
  // be reported as "not found", and must never silently pick one.
  AActor *Found = nullptr;
  if (!ActorNameParamUtils::ResolveActorOrSendError(Ctx, nullptr, TargetName, Found)) {
    return true;
  }

  UActorComponent *TargetComponent = nullptr;
  for (UActorComponent *Comp : Found->GetComponents()) {
    if (!Comp)
      continue;
    if (Comp->GetName().Equals(ComponentName, ESearchCase::IgnoreCase)) {
      TargetComponent = Comp;
      break;
    }
  }

  if (!TargetComponent) {
    Ctx.SendError(TEXT("COMPONENT_NOT_FOUND"), TEXT("Component not found"));
    return true;
  }

  TArray<FString> AppliedProperties;
  TArray<FString> PropertyWarnings;
  // Properties for which the engine's own change notification was fired. Reported
  // separately from `applied` because they are different facts: `applied` says the
  // value is in the field, `notified` says the class's PostEditChangeProperty ran, and
  // for a derived-state property only the second one moves anything. The engine setters
  // used above (Mobility, SimulatePhysics, StaticMesh/SkinnedAsset, and the shape extents)
  // are supersets of the notification and deliberately do not appear here.
  TArray<FString> NotifiedProperties;
  // What the per-instance bodies of an ISM/HISM - the objects the physics scene consults -
  // report AFTER a BodyInstance collision write. Empty for every other write.
  PinWright::FBodyInstanceCollisionMeasurement CollisionMeasurement;
  UClass *ComponentClass = TargetComponent->GetClass();
  TargetComponent->Modify();

  // PRIORITY: Apply Mobility FIRST.
  const TSharedPtr<FJsonValue> *MobilityVal = nullptr;
  FString MobilityKey;
  for (const auto &Pair : (*PropertiesPtr)->Values) {
    if (EARGCompat::JsonKeyToString(Pair.Key).Equals(TEXT("Mobility"), ESearchCase::IgnoreCase)) {
      MobilityVal = &Pair.Value;
      MobilityKey = EARGCompat::JsonKeyToString(Pair.Key);
      break;
    }
  }

  if (MobilityVal) {
    if (USceneComponent *SC = Cast<USceneComponent>(TargetComponent)) {
      FString EnumVal;
      if ((*MobilityVal)->TryGetString(EnumVal)) {
        int64 Val =
            StaticEnum<EComponentMobility::Type>()->GetValueByNameString(
                EnumVal);
        if (Val != INDEX_NONE) {
          SC->SetMobility((EComponentMobility::Type)Val);
          AppliedProperties.Add(MobilityKey);
          UE_LOG(LogPinWrightSubsystem, Display,
                 TEXT("Explicitly set Mobility to %s"), *EnumVal);
        }
      } else {
        double Val;
        if ((*MobilityVal)->TryGetNumber(Val)) {
          SC->SetMobility((EComponentMobility::Type)(int32)Val);
          AppliedProperties.Add(MobilityKey);
          UE_LOG(LogPinWrightSubsystem, Display,
                 TEXT("Explicitly set Mobility to %d"), (int32)Val);
        }
      }
    }
  }

  for (const auto &Pair : (*PropertiesPtr)->Values) {
    // Skip Mobility as we already handled it
    if (EARGCompat::JsonKeyToString(Pair.Key).Equals(TEXT("Mobility"), ESearchCase::IgnoreCase))
      continue;

    // Special handling for SimulatePhysics
    if (EARGCompat::JsonKeyToString(Pair.Key).Equals(TEXT("SimulatePhysics"), ESearchCase::IgnoreCase) ||
        EARGCompat::JsonKeyToString(Pair.Key).Equals(TEXT("bSimulatePhysics"), ESearchCase::IgnoreCase)) {
      if (UPrimitiveComponent *Prim =
              Cast<UPrimitiveComponent>(TargetComponent)) {
        bool bVal = false;
        if (Pair.Value->TryGetBool(bVal)) {
          Prim->SetSimulatePhysics(bVal);
          AppliedProperties.Add(EARGCompat::JsonKeyToString(Pair.Key));
          UE_LOG(LogPinWrightSubsystem, Display,
                 TEXT("Explicitly set SimulatePhysics to %s"),
                 bVal ? TEXT("True") : TEXT("False"));
          continue;
        }
      }
    }

    FProperty *Property = ComponentClass->FindPropertyByName(*Pair.Key);
    if (!Property) {
      PropertyWarnings.Add(
          FString::Printf(TEXT("Property not found: %s"), *Pair.Key));
      continue;
    }
    FString ApplyError;
    // A raw reflection write to StaticMesh / SkinnedAsset leaves the engine's
    // private shadow copy of that asset stale, which the UpdateComponentToWorld()
    // below turns into `Ensure condition failed: KnownStaticMesh == StaticMesh`
    // and a component whose render/streaming/physics state describes the OLD mesh.
    // Route those two through the engine's own setter first; everything else falls
    // through unchanged. Full reasoning: Utils/ComponentAssetPropertyWrite.h.
    const PinWright::EComponentAssetWrite AssetWrite =
        PinWright::ApplyComponentAssetProperty(TargetComponent, Property, Pair.Value,
                                               ApplyError);
    // The same "the store is not the whole write" shape, one layer down. A shape
    // component's extent DOES have a change hook, and firing it is not enough: it only
    // refreshes the body SETUP, leaving the live Chaos body at the old size, so the
    // renderer and every trace/overlap disagree about the same actor. The typed setter is
    // the superset here too. Full reasoning: Utils/ShapeExtentPropertyWrite.h.
    const PinWright::EShapeExtentWrite ExtentWrite =
        AssetWrite == PinWright::EComponentAssetWrite::NotApplicable
            ? PinWright::ApplyShapeExtentProperty(TargetComponent, Property, Pair.Value,
                                                  ApplyError)
            : PinWright::EShapeExtentWrite::NotApplicable;
    // The same shape again, and the one case where the typed setter is measured
    // INSUFFICIENT on its own. A BodyInstance collision write is stored raw today, so the
    // engine's own SetCollisionResponseToChannel / SetCollisionProfileName /
    // SetCollisionEnabled never run - and on an ISM/HISM even running them reaches only the
    // template, because the per-instance bodies are a one-time COPY of it. Route the write
    // and then push it onto those bodies, and publish what they measure rather than what the
    // template says. Full reasoning: Utils/BodyInstanceCollisionPropertyWrite.h.
    const PinWright::EBodyInstanceCollisionWrite CollisionWrite =
        (AssetWrite == PinWright::EComponentAssetWrite::NotApplicable &&
         ExtentWrite == PinWright::EShapeExtentWrite::NotApplicable)
            ? PinWright::ApplyBodyInstanceCollisionProperty(TargetComponent, Property,
                                                            Pair.Value, CollisionMeasurement,
                                                            ApplyError)
            : PinWright::EBodyInstanceCollisionWrite::NotApplicable;
    if (AssetWrite == PinWright::EComponentAssetWrite::Applied ||
        ExtentWrite == PinWright::EShapeExtentWrite::Applied ||
        CollisionWrite == PinWright::EBodyInstanceCollisionWrite::Applied)
      AppliedProperties.Add(EARGCompat::JsonKeyToString(Pair.Key));
    else if (AssetWrite == PinWright::EComponentAssetWrite::Failed ||
             ExtentWrite == PinWright::EShapeExtentWrite::Failed ||
             CollisionWrite == PinWright::EBodyInstanceCollisionWrite::Failed)
      PropertyWarnings.Add(FString::Printf(TEXT("Failed to set %s: %s"),
                                           *Pair.Key, *ApplyError));
    else if (ApplyJsonValueToProperty(TargetComponent, Property, Pair.Value,
                                      ApplyError)) {
      AppliedProperties.Add(EARGCompat::JsonKeyToString(Pair.Key));
      // The store is NOT the whole write. For any property whose effect lives in the
      // class's PostEditChangeProperty override - a water body's WaterMaterial, a sky
      // light's cubemap, a primitive's LDMaxDrawDistance - a raw reflection store runs
      // none of it, and MarkRenderStateDirty() below cannot
      // substitute (it recreates the scene proxy from the same stale derived state).
      // Reasoning, the measured water repro, and why the NON-CHAIN form with no
      // PreEditChange: Utils/PropertyChangeNotify.h.
      if (PinWright::NotifyPropertyChanged(TargetComponent, Property))
        NotifiedProperties.Add(EARGCompat::JsonKeyToString(Pair.Key));
    }
    else
      PropertyWarnings.Add(FString::Printf(TEXT("Failed to set %s: %s"),
                                           *Pair.Key, *ApplyError));

    // A notification cannot destroy the component (PropertyChangeNotify.h: no
    // PreEditChange means no reregister context, so ConsolidatedPostEditChange never
    // reruns construction scripts). Checked anyway, because the alternative to a cheap
    // guard here is a use-after-free in the next iteration.
    if (!IsValid(TargetComponent))
      break;
  }

  if (IsValid(TargetComponent)) {
    if (USceneComponent *SceneComponent =
            Cast<USceneComponent>(TargetComponent)) {
      SceneComponent->MarkRenderStateDirty();
      SceneComponent->UpdateComponentToWorld();
    }
    TargetComponent->MarkPackageDirty();
  }

  DiscloseAutoActivateDisabled(TargetComponent, *PropertiesPtr, PropertyWarnings);

  TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
  // Publishes `instanceBodies` (count / refreshed / requested / measured) for a collision
  // write on an instanced component, and warns when the bodies disagree with the request or
  // cannot be inspected. Adds nothing for any other write. Must run before the `warnings`
  // array below is serialised.
  PinWright::AddBodyInstanceCollisionReport(CollisionMeasurement, Data, PropertyWarnings);
  if (AppliedProperties.Num() > 0) {
    TArray<TSharedPtr<FJsonValue>> PropsArray;
    for (const FString &PropName : AppliedProperties)
      PropsArray.Add(MakeShared<FJsonValueString>(PropName));
    Data->SetArrayField(TEXT("applied"), PropsArray);
  }
  // PropertyWarnings was collected and then DROPPED: every per-property failure -
  // an unknown name, a value the importer could not convert, a setter that declined
  // - vanished, and the call reported success with an `applied` list that silently
  // omitted them. The verb's own registered summary already promised "Per-property
  // failures returned in 'warnings'", and the sibling actor.add_component emits
  // exactly this array, so the response was contradicting both its documentation and
  // its neighbour (rpc-design.md: report only what happened).
  if (PropertyWarnings.Num() > 0) {
    TArray<TSharedPtr<FJsonValue>> WarnArray;
    for (const FString &Warning : PropertyWarnings)
      WarnArray.Add(MakeShared<FJsonValueString>(Warning));
    Data->SetArrayField(TEXT("warnings"), WarnArray);
  }
  if (NotifiedProperties.Num() > 0) {
    TArray<TSharedPtr<FJsonValue>> NotifiedArray;
    for (const FString &PropName : NotifiedProperties)
      NotifiedArray.Add(MakeShared<FJsonValueString>(PropName));
    Data->SetArrayField(TEXT("notified"), NotifiedArray);
  }

  AddActorVerification(Data, Found);

  UE_LOG(LogPinWrightSubsystem, Display,
         TEXT("actor.set_component_properties: Updated properties for component '%s' on '%s'"),
         *TargetComponent->GetName(), *Found->GetActorLabel());
  Ctx.SendSuccess(Data);
  return true;
}

// ---- actor.get_components ----
REGISTER_RPC_HANDLER("actor.get_components", "actor", "List every component on an actor instance (or, if a Blueprint asset path is passed, the BP's CDO components). Each entry includes class path, name, and (for scene components) relative location/rotation/scale.",
    RPC_PARAMS(
        ActorNameParamUtils::ActorNameParamReq(TEXT("string"), TEXT("Either an actor display label/name in the level, or a Blueprint asset path (e.g. /Game/Foo/BP_Bar) to inspect the class default object's components. The objectPath and actorPath aliases (the key spawn/duplicate return) are also accepted.")),
        RPC_PARAM_OPT("nameMatch", "string", "Case-insensitive substring filter on component name. Snake_case name_match accepted."),
        RPC_PARAM_OPT("name_match", "string", "Snake_case alias for nameMatch."),
        RPC_PARAM_OPT("componentClass", "classref", "Component UClass name or path; matches that class and subclasses. Snake_case component_class accepted."),
        RPC_PARAM_OPT("component_class", "classref", "Snake_case alias for componentClass.")
    ))
{
  FString TargetName;
  if (!ActorNameParamUtils::RequireActorName(Ctx, TargetName)) {
    return true;
  }

  FComponentReadFilter ComponentFilter;
  FString FilterErrorCode;
  FString FilterErrorMessage;
  if (!TryParseComponentReadFilter(Ctx.GetRawPayload(), ComponentFilter,
      FilterErrorCode, FilterErrorMessage)) {
    Ctx.SendError(FilterErrorCode, FilterErrorMessage);
    return true;
  }

  AActor *Found = McpActorUtils::FindActorByName(nullptr, TargetName);
  // Fallback: Check if it's a Blueprint asset to inspect CDO components. Gate the
  // load on a quiet registry-existence probe first: ResolveAsset
  // logs an editor-level error for any name that isn't a loadable asset, so calling
  // it unconditionally on a missing actor name (the common case — a label typo, or a
  // path-shaped key with no asset behind it) spams the log with a misleading
  // "LoadAsset failed" line. ResolveAsset is a registry-only lookup that returns
  // false silently, so only names that actually resolve to an asset reach LoadAsset.
  if (!Found && ResolveAsset(TargetName).bExists) {
    if (UObject *Asset = ResolveAsset(TargetName, /*bLoadObject=*/true).Object) {
      if (UBlueprint *BP = Cast<UBlueprint>(Asset)) {
        if (BP->GeneratedClass) {
          Found = Cast<AActor>(BP->GeneratedClass->GetDefaultObject());
        }
      }
    }
  }

  if (!Found) {
    Ctx.SendError(TEXT("ACTOR_NOT_FOUND"), TEXT("Actor or Blueprint not found"));
    return true;
  }

  TArray<TSharedPtr<FJsonValue>> ComponentsArray;
  for (UActorComponent *Comp : Found->GetComponents()) {
    if (!Comp)
      continue;
    if (!ComponentFilter.Matches(Comp))
      continue;
    TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
    Entry->SetStringField(TEXT("name"), Comp->GetName());
    Entry->SetStringField(TEXT("class"), Comp->GetClass()
                                             ? Comp->GetClass()->GetPathName()
                                             : TEXT(""));
    Entry->SetStringField(TEXT("path"), Comp->GetPathName());
    if (USceneComponent *SceneComp = Cast<USceneComponent>(Comp)) {
      FVector Loc = SceneComp->GetRelativeLocation();
      FRotator Rot = SceneComp->GetRelativeRotation();
      FVector Scale = SceneComp->GetRelativeScale3D();

      TSharedPtr<FJsonObject> LocObj = MakeShared<FJsonObject>();
      LocObj->SetNumberField(TEXT("x"), Loc.X);
      LocObj->SetNumberField(TEXT("y"), Loc.Y);
      LocObj->SetNumberField(TEXT("z"), Loc.Z);
      Entry->SetObjectField(TEXT("relativeLocation"), LocObj);

      TSharedPtr<FJsonObject> RotObj = MakeShared<FJsonObject>();
      RotObj->SetNumberField(TEXT("pitch"), Rot.Pitch);
      RotObj->SetNumberField(TEXT("yaw"), Rot.Yaw);
      RotObj->SetNumberField(TEXT("roll"), Rot.Roll);
      Entry->SetObjectField(TEXT("relativeRotation"), RotObj);

      TSharedPtr<FJsonObject> ScaleObj = MakeShared<FJsonObject>();
      ScaleObj->SetNumberField(TEXT("x"), Scale.X);
      ScaleObj->SetNumberField(TEXT("y"), Scale.Y);
      ScaleObj->SetNumberField(TEXT("z"), Scale.Z);
      Entry->SetObjectField(TEXT("relativeScale"), ScaleObj);
    }
    ComponentsArray.Add(MakeShared<FJsonValueObject>(Entry));
  }

  TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
  Data->SetArrayField(TEXT("components"), ComponentsArray);
  Data->SetNumberField(TEXT("count"), ComponentsArray.Num());

  if (Found) {
    AddActorVerification(Data, Found);
  }

  Ctx.SendSuccess(Data);
  return true;
}

// ---- actor.remove_component ----
REGISTER_RPC_HANDLER("actor.remove_component", "actor", "Destroy an instance-level component on a placed actor by name (case-insensitive match). Use blueprint.scs.remove_component to remove a class-template component instead.",
    RPC_PARAMS(
        ParamAliasUtils::MakeAliasParamSpec(TEXT("actorName"), TEXT("string"),
            TEXT("Display label or name of the actor (snake_case actor_name also accepted)."),
            /*bRequired=*/true, TArray<FString>({TEXT("actorName"), TEXT("actor_name")})),
        ParamAliasUtils::MakeAliasParamSpec(TEXT("componentName"), TEXT("string"),
            TEXT("Component identifier to destroy; matched case-insensitively. Snake_case component_name accepted."),
            /*bRequired=*/true, TArray<FString>({TEXT("componentName"), TEXT("component_name")}))
    ))
{
  FString ActorName = Ctx.GetStringFirstOf({TEXT("actorName"), TEXT("actor_name")});

  FString ComponentName = Ctx.GetStringFirstOf({TEXT("componentName"), TEXT("component_name")});

  if (ActorName.IsEmpty()) {
    Ctx.SendError(TEXT("MISSING_PARAM"), TEXT("actorName is required"));
    return true;
  }

  if (ComponentName.IsEmpty()) {
    Ctx.SendError(TEXT("MISSING_PARAM"), TEXT("componentName is required"));
    return true;
  }

  // Resolve with an explicit ambiguity verdict: a label matching several actors must not
  // be reported as "not found", and must never silently pick one.
  AActor* Actor = nullptr;
  if (!ActorNameParamUtils::ResolveActorOrSendError(Ctx, nullptr, ActorName, Actor)) {
    return true;
  }

  TInlineComponentArray<UActorComponent*> Components;
  Actor->GetComponents(Components);

  for (UActorComponent* Component : Components) {
    if (Component && Component->GetName().Equals(ComponentName, ESearchCase::IgnoreCase)) {
      Component->DestroyComponent();
      TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
      Data->SetStringField(TEXT("actorName"), ActorName);
      Data->SetStringField(TEXT("componentName"), ComponentName);
      Data->SetBoolField(TEXT("existsAfter"), false);
      Data->SetStringField(TEXT("action"), TEXT("control_actor:deleted"));
      Ctx.SendSuccess(Data);
      return true;
    }
  }

  Ctx.SendError(TEXT("COMPONENT_NOT_FOUND"),
      FString::Printf(TEXT("Component not found: %s"), *ComponentName));
  return true;
}

// ---- actor.get_component_property ----
REGISTER_RPC_HANDLER("actor.get_component_property", "actor", "Read a single UPROPERTY value from a named component of a placed actor and return it as JSON. Uses property reflection to serialize structs/arrays/objects; complex containers may yield valueExportError. propertyName supports dotted nested paths (e.g. 'BodyInstance.CollisionEnabled') that hop into a struct/object member and return just the scalar leaf — use this to read one field of a large struct without serializing (and spilling) the whole struct.",
    RPC_PARAMS(
        RPC_PARAM_REQ("actorName", "string", "Display label or name of the actor." ACTORNAME_COLLISION_STEER),
        RPC_PARAM_REQ("componentName", "string", "Component identifier on the actor (case-insensitive)."),
        RPC_PARAM_REQ("propertyName", "string", "UPROPERTY name on the component class (case-sensitive FName lookup), OR a dotted nested path ('BodyInstance.CollisionEnabled') to read a single sub-field of a struct/object property and return only that leaf value instead of the whole struct.")
    ))
{
  FString ActorName = Ctx.GetString(TEXT("actorName"));
  FString ComponentName = Ctx.GetString(TEXT("componentName"));
  FString PropertyName = Ctx.GetString(TEXT("propertyName"));

  if (ActorName.IsEmpty() || ComponentName.IsEmpty() || PropertyName.IsEmpty()) {
    Ctx.SendError(TEXT("MISSING_PARAM"), TEXT("actorName, componentName, and propertyName are required"));
    return true;
  }

  // Resolve with an explicit ambiguity verdict: a label matching several actors must not
  // be reported as "not found", and must never silently pick one.
  AActor* Actor = nullptr;
  if (!ActorNameParamUtils::ResolveActorOrSendError(Ctx, nullptr, ActorName, Actor)) {
    return true;
  }

  TInlineComponentArray<UActorComponent*> Components;
  Actor->GetComponents(Components);

  for (UActorComponent* Component : Components) {
    if (Component && Component->GetName().Equals(ComponentName, ESearchCase::IgnoreCase)) {
      // Resolve the property via the shared dispatch primitive. A dotted
      // propertyName ("BodyInstance.CollisionEnabled") walks into a struct/object
      // member (the same resolver property.get/property.set use) and returns just
      // the leaf — so reading one scalar of a large struct (e.g. FBodyInstance)
      // returns that one value instead of serializing the whole decomposed struct
      // and spilling to disk. A non-dotted propertyName keeps the original
      // whole-property behavior unchanged.
      void* Container = nullptr;
      FString ResolveError;
      FPropertyNotifyTarget ExportTarget;
      ExportTarget.Object = Component;
      ExportTarget.RelativePath = PropertyName;
      FProperty* Property = PropertyName.Contains(TEXT("."))
          ? ResolveNestedPropertyPath(
              Component, PropertyName, Container, ResolveError, &ExportTarget)
          : ResolvePropertyOnObject(Component, PropertyName, Container, ResolveError);
      if (Property) {
        TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
        Data->SetStringField(TEXT("actorName"), ActorName);
        Data->SetStringField(TEXT("componentName"), ComponentName);
        Data->SetStringField(TEXT("propertyName"), PropertyName);
        const FPropertyExportSource ExportSource = FPropertyExportSource::FromResolvedContainer(
            Container, ExportTarget.Object);
        if (TSharedPtr<FJsonValue> CurrentValue = ExportPropertyToJsonValue(ExportSource, Property)) {
          Data->SetField(TEXT("value"), CurrentValue);
        } else {
          Data->SetStringField(TEXT("valueExportError"),
              FString::Printf(TEXT("Unable to export property value for '%s'"), *PropertyName));
        }
        Ctx.SendSuccess(Data);
        return true;
      }
      // Component matched but the property didn't resolve; report the specific
      // resolver error (e.g. a bad dotted segment) rather than the generic
      // "not found" used when no component matched at all.
      Ctx.SendError(TEXT("NOT_FOUND"),
          FString::Printf(TEXT("Property '%s' not found on component '%s': %s"),
              *PropertyName, *ComponentName, *ResolveError));
      return true;
    }
  }

  Ctx.SendError(TEXT("NOT_FOUND"), TEXT("Component or property not found"));
  return true;
}
