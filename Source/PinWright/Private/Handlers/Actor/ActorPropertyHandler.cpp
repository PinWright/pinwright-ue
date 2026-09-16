// Copyright (c) 2026 Alexander Penkin. MIT License.

// ActorPropertyHandler.cpp - Migrated from PinWright_ControlHandlers.cpp
// Handles actor.set_visibility, actor.apply_force, actor.set_collision,
// actor.add_tag, actor.remove_tag, actor.set_blueprint_variables,
// actor.attach, actor.detach,
// actor.create_snapshot, actor.restore_snapshot

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ParamSpec.h"
#include "Handlers/ParamAliasUtils.h"
#include "PinWrightGlobals.h"
#include "PinWrightHelpers.h"
#include "PinWrightSubsystem.h"
#include "Utils/ActorUtils.h"
#include "Handlers/Actor/ActorNameParamUtils.h"
#include "Dom/JsonObject.h"
#include "Compat/JsonKeyCompat.h"

#include "Editor.h"
#if __has_include("Subsystems/EditorActorSubsystem.h")
#include "Subsystems/EditorActorSubsystem.h"
#elif __has_include("EditorActorSubsystem.h")
#include "EditorActorSubsystem.h"
#endif
#include "Components/PrimitiveComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/SceneComponent.h"
#include "Engine/StaticMesh.h"
#include "GameFramework/Actor.h"

// ---- actor.set_visibility ----
REGISTER_RPC_HANDLER("actor.set_visibility", "actor", "Toggle whether an actor is hidden in the level. Also flips collision (visible=true enables collision, visible=false disables) and propagates the visibility flag to all primitive components, then marks the package dirty.",
    RPC_PARAMS(
        RPC_PARAM_REQ("actorName", "string", "Display label or name of the actor in the current level."),
        RPC_PARAM_OPT("visible", "boolean", "true (default) makes the actor visible and re-enables collision; false hides it and disables collision.")
    ))
{
  FString TargetName = Ctx.GetString(TEXT("actorName"));
  if (TargetName.IsEmpty()) {
    Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("actorName required"));
    return true;
  }

  bool bVisible = Ctx.GetBool(TEXT("visible"), true);

  // Resolve with an explicit ambiguity verdict: a label matching several actors must not
  // be reported as "not found", and must never silently pick one.
  AActor *Found = nullptr;
  if (!ActorNameParamUtils::ResolveActorOrSendError(Ctx, nullptr, TargetName, Found)) {
    return true;
  }

  Found->Modify();
  Found->SetActorHiddenInGame(!bVisible);
  Found->SetActorEnableCollision(bVisible);

  for (UActorComponent *Comp : Found->GetComponents()) {
    if (!Comp)
      continue;
    if (UPrimitiveComponent *Prim = Cast<UPrimitiveComponent>(Comp)) {
      Prim->SetVisibility(bVisible, true);
      Prim->SetHiddenInGame(!bVisible);
    }
  }

  Found->MarkComponentsRenderStateDirty();
  Found->MarkPackageDirty();

  const bool bIsHidden = Found->IsHidden();
  const bool bStateMatches = (bIsHidden == !bVisible);

  TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
  Data->SetBoolField(TEXT("visible"), !bIsHidden);
  Data->SetStringField(TEXT("actorName"), Found->GetActorLabel());

  if (!bStateMatches) {
    Ctx.SendError(TEXT("VISIBILITY_MISMATCH"), TEXT("Failed to set actor visibility"));
    return true;
  }

  AddActorVerification(Data, Found);

  UE_LOG(LogPinWrightSubsystem, Display,
         TEXT("actor.set_visibility: Set visibility to %s for '%s'"),
         bVisible ? TEXT("True") : TEXT("False"), *Found->GetActorLabel());
  Ctx.SendSuccess(Data);
  return true;
}

// ---- actor.apply_force ----
REGISTER_RPC_HANDLER("actor.apply_force", "actor", "Apply an instantaneous physics force (in unreal units, world space) to the actor's first primitive component. Auto-promotes mobility to Movable, enables collision, and turns on physics simulation if needed; fails if the static mesh has no collision body.",
    RPC_PARAMS(
        RPC_PARAM_REQ("actorName", "string", "Display label or name of the actor." ACTORNAME_COLLISION_STEER),
        RPC_PARAM_REQ("force", "object", "Force vector in world space as {x,y,z}; magnitude is in cm/s^2 * mass per UE physics convention.")
    ))
{
  FString TargetName = Ctx.GetString(TEXT("actorName"));
  FVector ForceVector = Ctx.GetVector(TEXT("force"), FVector::ZeroVector);

  // Resolve with an explicit ambiguity verdict: a label matching several actors must not
  // be reported as "not found", and must never silently pick one.
  AActor *Found = nullptr;
  if (!ActorNameParamUtils::ResolveActorOrSendError(Ctx, nullptr, TargetName, Found)) {
    return true;
  }

  UPrimitiveComponent *Prim =
      Found->FindComponentByClass<UPrimitiveComponent>();
  if (!Prim) {
    if (UStaticMeshComponent *SMC =
            Found->FindComponentByClass<UStaticMeshComponent>())
      Prim = SMC;
  }

  if (!Prim) {
    Ctx.SendError(TEXT("NO_COMPONENT"), TEXT("No component to apply force"));
    return true;
  }

  if (Prim->Mobility == EComponentMobility::Static)
    Prim->SetMobility(EComponentMobility::Movable);

  if (Prim->GetCollisionEnabled() == ECollisionEnabled::NoCollision) {
    Prim->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
  }

  if (UStaticMeshComponent *SMC = Cast<UStaticMeshComponent>(Prim)) {
    if (!SMC->GetStaticMesh()) {
      Ctx.SendError(TEXT("PHYSICS_FAILED"),
          TEXT("StaticMeshComponent has no StaticMesh assigned."));
      return true;
    }
    if (!SMC->GetStaticMesh()->GetBodySetup()) {
      Ctx.SendError(TEXT("PHYSICS_FAILED"),
          TEXT("StaticMesh has no collision geometry (BodySetup is null)."));
      return true;
    }
  }

  if (!Prim->IsSimulatingPhysics()) {
    Prim->SetSimulatePhysics(true);
    Prim->RecreatePhysicsState();
  }

  Prim->AddForce(ForceVector);
  Prim->WakeAllRigidBodies();
  Prim->MarkRenderStateDirty();

  const bool bIsSimulating = Prim->IsSimulatingPhysics();

  TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
  Data->SetBoolField(TEXT("simulating"), bIsSimulating);
  TArray<TSharedPtr<FJsonValue>> Applied;
  Applied.Add(MakeShared<FJsonValueNumber>(ForceVector.X));
  Applied.Add(MakeShared<FJsonValueNumber>(ForceVector.Y));
  Applied.Add(MakeShared<FJsonValueNumber>(ForceVector.Z));
  Data->SetArrayField(TEXT("applied"), Applied);
  Data->SetStringField(TEXT("actorName"), Found->GetActorLabel());

  if (!bIsSimulating) {
    FString FailureReason = TEXT("Failed to enable physics simulation.");
    if (Prim->GetCollisionEnabled() == ECollisionEnabled::NoCollision) {
      FailureReason += TEXT(" Collision is disabled.");
    } else if (Prim->Mobility != EComponentMobility::Movable) {
      FailureReason += TEXT(" Component is not Movable.");
    }
    Ctx.SendError(TEXT("PHYSICS_FAILED"), FailureReason);
    return true;
  }

  AddActorVerification(Data, Found);

  UE_LOG(LogPinWrightSubsystem, Display,
         TEXT("actor.apply_force: Applied force to '%s'"), *Found->GetActorLabel());
  Ctx.SendSuccess(Data);
  return true;
}

// ---- actor.set_collision ----
REGISTER_RPC_HANDLER("actor.set_collision", "actor", "Toggle collision on the actor's root primitive component between QueryAndPhysics (enabled) and NoCollision (disabled). Affects only the root; component-level collision channels are not touched.",
    RPC_PARAMS(
        ParamAliasUtils::MakeAliasParamSpec(TEXT("actorName"), TEXT("string"),
            TEXT("Display label or name of the actor (also accepts snake_case actor_name)."),
            /*bRequired=*/true, TArray<FString>({TEXT("actorName"), TEXT("actor_name")})),
        RPC_PARAM_OPT_ALIAS("collisionEnabled", "boolean", "true (default) sets QueryAndPhysics; false sets NoCollision. Snake_case alias collision_enabled also accepted.", "collision_enabled")
    ))
{
  FString ActorName = Ctx.GetStringFirstOf({TEXT("actorName"), TEXT("actor_name")});

  const bool bCollisionEnabled =
      Ctx.GetBoolFirstOf({TEXT("collisionEnabled"), TEXT("collision_enabled")}, true);

  if (ActorName.IsEmpty()) {
    Ctx.SendError(TEXT("MISSING_PARAM"), TEXT("actorName is required"));
    return true;
  }

  AActor* Actor = McpActorUtils::FindActorByName(nullptr, ActorName);
  if (!Actor) {
    Ctx.SendError(TEXT("ACTOR_NOT_FOUND"),
        FString::Printf(TEXT("Actor not found: %s"), *ActorName));
    return true;
  }

  if (USceneComponent* RootComp = Actor->GetRootComponent()) {
    if (UPrimitiveComponent* PrimComp = Cast<UPrimitiveComponent>(RootComp)) {
      if (bCollisionEnabled) {
        PrimComp->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
      } else {
        PrimComp->SetCollisionEnabled(ECollisionEnabled::NoCollision);
      }
    }
  }

  TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
  Data->SetStringField(TEXT("actorName"), ActorName);
  Data->SetBoolField(TEXT("collisionEnabled"), bCollisionEnabled);
  Ctx.SendSuccess(Data);
  return true;
}

// ---- actor.add_tag ----
REGISTER_RPC_HANDLER("actor.add_tag", "actor", "Add an FName tag to the actor's Tags array (idempotent — adding an existing tag is a no-op and reports wasPresent=true).",
    RPC_PARAMS(
        RPC_PARAM_REQ("actorName", "string", "Display label or name of the actor." ACTORNAME_COLLISION_STEER),
        RPC_PARAM_REQ("tag", "string", "Tag string converted to FName; case-sensitive on lookup.")
    ))
{
  FString TargetName = Ctx.GetString(TEXT("actorName"));
  FString TagValue = Ctx.GetString(TEXT("tag"));
  if (TargetName.IsEmpty() || TagValue.IsEmpty()) {
    Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("actorName and tag required"));
    return true;
  }

  // Resolve with an explicit ambiguity verdict: a label matching several actors must not
  // be reported as "not found", and must never silently pick one.
  AActor *Found = nullptr;
  if (!ActorNameParamUtils::ResolveActorOrSendError(Ctx, nullptr, TargetName, Found)) {
    return true;
  }

  const FName TagName(*TagValue);
  const bool bAlreadyHad = Found->Tags.Contains(TagName);

  Found->Modify();
  Found->Tags.AddUnique(TagName);
  Found->MarkPackageDirty();

  TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
  Data->SetBoolField(TEXT("wasPresent"), bAlreadyHad);
  Data->SetStringField(TEXT("actorName"), Found->GetActorLabel());
  Data->SetStringField(TEXT("tag"), TagName.ToString());

  AddActorVerification(Data, Found);

  UE_LOG(LogPinWrightSubsystem, Display,
         TEXT("actor.add_tag: Added tag '%s' to '%s'"), *TagName.ToString(),
         *Found->GetActorLabel());
  Ctx.SendSuccess(Data);
  return true;
}

// ---- actor.remove_tag ----
REGISTER_RPC_HANDLER("actor.remove_tag", "actor", "Remove an FName tag from the actor's Tags array. Idempotent — removing an absent tag returns success with wasPresent=false.",
    RPC_PARAMS(
        RPC_PARAM_REQ("actorName", "string", "Display label or name of the actor." ACTORNAME_COLLISION_STEER),
        RPC_PARAM_REQ("tag", "string", "Tag string to remove; case-sensitive match against existing FName tags.")
    ))
{
  FString TargetName = Ctx.GetString(TEXT("actorName"));
  FString TagValue = Ctx.GetString(TEXT("tag"));
  if (TargetName.IsEmpty() || TagValue.IsEmpty()) {
    Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("actorName and tag required"));
    return true;
  }

  // Resolve with an explicit ambiguity verdict: a label matching several actors must not
  // be reported as "not found", and must never silently pick one.
  AActor *Found = nullptr;
  if (!ActorNameParamUtils::ResolveActorOrSendError(Ctx, nullptr, TargetName, Found)) {
    return true;
  }

  const FName TagName(*TagValue);
  if (!Found->Tags.Contains(TagName)) {
    // Idempotent success
    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetBoolField(TEXT("success"), true);
    Resp->SetBoolField(TEXT("wasPresent"), false);
    Resp->SetStringField(TEXT("actorName"), Found->GetActorLabel());
    Resp->SetStringField(TEXT("tag"), TagValue);
    Ctx.SendSuccess(Resp);
    return true;
  }

  Found->Modify();
  Found->Tags.Remove(TagName);
  Found->MarkPackageDirty();

  TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
  Data->SetBoolField(TEXT("wasPresent"), true);
  Data->SetStringField(TEXT("actorName"), Found->GetActorLabel());
  Data->SetStringField(TEXT("tag"), TagValue);

  AddActorVerification(Data, Found);

  UE_LOG(LogPinWrightSubsystem, Display,
         TEXT("actor.remove_tag: Removed tag '%s' from '%s'"), *TagValue,
         *Found->GetActorLabel());
  Ctx.SendSuccess(Data);
  return true;
}

// ---- actor.set_blueprint_variables ----
REGISTER_RPC_HANDLER("actor.set_blueprint_variables", "actor", "Set one or more Blueprint UPROPERTY values on a placed actor instance (level instance, not the BP class default). Reports per-variable Applied / Warnings; use blueprint.set_default to mutate the class default instead.",
    RPC_PARAMS(
        RPC_PARAM_REQ("actorName", "string", "Display label or name of the actor instance in the level."),
        RPC_PARAM_REQ("variables", "object", "Object whose keys are property names (FName-matched on the actor's class) and values are JSON-encoded property values.")
    ))
{
  auto* Subsystem = Ctx.GetSubsystem();
  const auto& Payload = Ctx.GetRawPayload();

  FString TargetName = Ctx.GetString(TEXT("actorName"));
  if (TargetName.IsEmpty()) {
    Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("actorName required"));
    return true;
  }

  const TSharedPtr<FJsonObject> *VariablesPtr = nullptr;
  if (!(Payload->TryGetObjectField(TEXT("variables"), VariablesPtr) &&
        VariablesPtr && VariablesPtr->IsValid())) {
    Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("variables object required"));
    return true;
  }

  // Resolve with an explicit ambiguity verdict: a label matching several actors must not
  // be reported as "not found", and must never silently pick one.
  AActor *Found = nullptr;
  if (!ActorNameParamUtils::ResolveActorOrSendError(Ctx, nullptr, TargetName, Found)) {
    return true;
  }

  UClass *ActorClass = Found->GetClass();
  Found->Modify();
  TArray<FString> Applied;
  TArray<FString> Warnings;

  for (const auto &Pair : (*VariablesPtr)->Values) {
    FProperty *Property = ActorClass->FindPropertyByName(*Pair.Key);
    if (!Property) {
      Warnings.Add(FString::Printf(TEXT("Property not found: %s"), *Pair.Key));
      continue;
    }

    FString ApplyError;
    if (ApplyJsonValueToProperty(Found, Property, Pair.Value, ApplyError))
      Applied.Add(EARGCompat::JsonKeyToString(Pair.Key));
    else
      Warnings.Add(FString::Printf(TEXT("Failed to set %s: %s"), *Pair.Key,
                                   *ApplyError));
  }

  Found->MarkComponentsRenderStateDirty();
  Found->MarkPackageDirty();

  TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
  if (Applied.Num() > 0) {
    TArray<TSharedPtr<FJsonValue>> AppliedArray;
    for (const FString &Name : Applied)
      AppliedArray.Add(MakeShared<FJsonValueString>(Name));
    Data->SetArrayField(TEXT("updated"), AppliedArray);
  }

  if (Warnings.Num() > 0) {
    TArray<TSharedPtr<FJsonValue>> WarnArray;
    for (const FString &Warning : Warnings)
      WarnArray.Add(MakeShared<FJsonValueString>(Warning));
    Data->SetArrayField(TEXT("warnings"), WarnArray);
  }

  Ctx.SendSuccess(Data);
  return true;
}

// ---- actor.attach ----
REGISTER_RPC_HANDLER("actor.attach", "actor", "Attach one actor to another using KeepWorldTransform (child stays at its current world location after attachment) and set the parent as the child's owner. Rejects self-attach; both actors must have a root component.",
    RPC_PARAMS(
        RPC_PARAM_REQ("childActor", "string", "Display label or name of the actor that becomes the attached child."),
        RPC_PARAM_REQ("parentActor", "string", "Display label or name of the actor that becomes the attachment parent.")
    ))
{
  FString ChildName = Ctx.GetString(TEXT("childActor"));
  FString ParentName = Ctx.GetString(TEXT("parentActor"));
  if (ChildName.IsEmpty() || ParentName.IsEmpty()) {
    Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("childActor and parentActor required"));
    return true;
  }

  AActor *Child = McpActorUtils::FindActorByName(nullptr, ChildName);
  AActor *Parent = McpActorUtils::FindActorByName(nullptr, ParentName);
  if (!Child || !Parent) {
    Ctx.SendError(TEXT("ACTOR_NOT_FOUND"), TEXT("Child or parent actor not found"));
    return true;
  }

  if (Child == Parent) {
    Ctx.SendError(TEXT("CYCLE_DETECTED"), TEXT("Cannot attach actor to itself"));
    return true;
  }

  USceneComponent *ChildRoot = Child->GetRootComponent();
  USceneComponent *ParentRoot = Parent->GetRootComponent();
  if (!ChildRoot || !ParentRoot) {
    Ctx.SendError(TEXT("ROOT_MISSING"), TEXT("Actor missing root component"));
    return true;
  }

  Child->Modify();
  ChildRoot->Modify();
  ChildRoot->AttachToComponent(ParentRoot,
                               FAttachmentTransformRules::KeepWorldTransform);
  Child->SetOwner(Parent);
  Child->MarkPackageDirty();
  Parent->MarkPackageDirty();

  bool bAttached = false;
  if (Child->GetRootComponent() &&
      Child->GetRootComponent()->GetAttachParent() == ParentRoot) {
    bAttached = true;
  }

  TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
  Data->SetStringField(TEXT("child"), Child->GetActorLabel());
  Data->SetStringField(TEXT("parent"), Parent->GetActorLabel());
  Data->SetBoolField(TEXT("attached"), bAttached);

  if (!bAttached) {
    Ctx.SendError(TEXT("ATTACH_FAILED"), TEXT("Failed to attach actor"));
    return true;
  }

  AddActorVerification(Data, Child);

  UE_LOG(LogPinWrightSubsystem, Display,
         TEXT("actor.attach: Attached '%s' to '%s'"), *Child->GetActorLabel(),
         *Parent->GetActorLabel());
  Ctx.SendSuccess(Data);
  return true;
}

// ---- actor.detach ----
REGISTER_RPC_HANDLER("actor.detach", "actor", "Detach an actor from its current parent using KeepWorldTransform (the actor stays at its current world location) and clears its owner. No-op success if already unattached.",
    RPC_PARAMS(
        RPC_PARAM_REQ("actorName", "string", "Display label or name of the actor to detach.")
    ))
{
  FString TargetName = Ctx.GetString(TEXT("actorName"));
  if (TargetName.IsEmpty()) {
    Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("actorName required"));
    return true;
  }

  // Resolve with an explicit ambiguity verdict: a label matching several actors must not
  // be reported as "not found", and must never silently pick one.
  AActor *Found = nullptr;
  if (!ActorNameParamUtils::ResolveActorOrSendError(Ctx, nullptr, TargetName, Found)) {
    return true;
  }

  USceneComponent *RootComp = Found->GetRootComponent();
  if (!RootComp || !RootComp->GetAttachParent()) {
    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetBoolField(TEXT("success"), true);
    Resp->SetStringField(TEXT("actorName"), Found->GetActorLabel());
    Resp->SetStringField(TEXT("note"), TEXT("Actor was not attached"));
    Ctx.SendSuccess(Resp);
    return true;
  }

  Found->Modify();
  RootComp->Modify();
  RootComp->DetachFromComponent(FDetachmentTransformRules::KeepWorldTransform);
  Found->SetOwner(nullptr);
  Found->MarkPackageDirty();

  const bool bDetached = (RootComp->GetAttachParent() == nullptr);

  TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
  Data->SetStringField(TEXT("actorName"), Found->GetActorLabel());
  Data->SetBoolField(TEXT("detached"), bDetached);

  if (!bDetached) {
    Ctx.SendError(TEXT("DETACH_FAILED"), TEXT("Failed to detach actor"));
    return true;
  }

  AddActorVerification(Data, Found);

  UE_LOG(LogPinWrightSubsystem, Display,
         TEXT("actor.detach: Detached '%s'"), *Found->GetActorLabel());
  Ctx.SendSuccess(Data);
  return true;
}

// ---- actor.create_snapshot ----
REGISTER_RPC_HANDLER("actor.create_snapshot", "actor", "Save the actor's current FTransform (location, rotation, scale) to a named in-memory snapshot keyed by ActorPath::SnapshotName. Snapshots persist for the editor session only and are lost on shutdown.",
    RPC_PARAMS(
        RPC_PARAM_REQ("actorName", "string", "Display label or name of the actor whose transform is being captured."),
        RPC_PARAM_REQ("snapshotName", "string", "Identifier for this snapshot; later passed to actor.restore_snapshot. Reusing the name overwrites the prior capture.")
    ))
{
  FString TargetName = Ctx.GetString(TEXT("actorName"));
  if (TargetName.IsEmpty()) {
    Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("actorName required"));
    return true;
  }

  FString SnapshotName = Ctx.GetString(TEXT("snapshotName"));
  if (SnapshotName.IsEmpty()) {
    Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("snapshotName required"));
    return true;
  }

  // Resolve with an explicit ambiguity verdict: a label matching several actors must not
  // be reported as "not found", and must never silently pick one.
  AActor *Found = nullptr;
  if (!ActorNameParamUtils::ResolveActorOrSendError(Ctx, nullptr, TargetName, Found)) {
    return true;
  }

  const FString SnapshotKey =
      FString::Printf(TEXT("%s::%s"), *Found->GetPathName(), *SnapshotName);
  FPluginState::Get().CachedActorSnapshots().Add(SnapshotKey, Found->GetActorTransform());

  TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
  Data->SetStringField(TEXT("snapshotName"), SnapshotName);
  Data->SetStringField(TEXT("actorName"), Found->GetActorLabel());
  Ctx.SendSuccess(Data);
  return true;
}

// ---- actor.restore_snapshot ----
REGISTER_RPC_HANDLER("actor.restore_snapshot", "actor", "Restore an actor to the FTransform previously captured by actor.create_snapshot for that actor + snapshot name. Errors with SNAPSHOT_NOT_FOUND if the named snapshot does not exist for this actor.",
    RPC_PARAMS(
        RPC_PARAM_REQ("actorName", "string", "Display label or name of the actor to restore."),
        RPC_PARAM_REQ("snapshotName", "string", "Snapshot identifier previously passed to actor.create_snapshot for this same actor.")
    ))
{
  FString TargetName = Ctx.GetString(TEXT("actorName"));
  if (TargetName.IsEmpty()) {
    Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("actorName required"));
    return true;
  }

  FString SnapshotName = Ctx.GetString(TEXT("snapshotName"));
  if (SnapshotName.IsEmpty()) {
    Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("snapshotName required"));
    return true;
  }

  // Resolve with an explicit ambiguity verdict: a label matching several actors must not
  // be reported as "not found", and must never silently pick one.
  AActor *Found = nullptr;
  if (!ActorNameParamUtils::ResolveActorOrSendError(Ctx, nullptr, TargetName, Found)) {
    return true;
  }

  const FString SnapshotKey =
      FString::Printf(TEXT("%s::%s"), *Found->GetPathName(), *SnapshotName);
  if (!FPluginState::Get().CachedActorSnapshots().Contains(SnapshotKey)) {
    Ctx.SendError(TEXT("SNAPSHOT_NOT_FOUND"), TEXT("Snapshot not found"));
    return true;
  }

  const FTransform &SavedTransform = FPluginState::Get().CachedActorSnapshots()[SnapshotKey];
  Found->Modify();
  Found->SetActorTransform(SavedTransform);
  Found->MarkComponentsRenderStateDirty();
  Found->MarkPackageDirty();

  TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
  Data->SetStringField(TEXT("snapshotName"), SnapshotName);
  Data->SetStringField(TEXT("actorName"), Found->GetActorLabel());
  Ctx.SendSuccess(Data);
  return true;
}
