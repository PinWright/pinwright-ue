// Copyright (c) 2026 Alexander Penkin. MIT License.

// ActorTransformHandler.cpp - Migrated from PinWright_ControlHandlers.cpp
// Handles actor.set_transform, actor.get_transform, actor.get_bounding_box

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ParamSpec.h"
#include "Handlers/Actor/ActorNameParamUtils.h"
#include "PinWrightGlobals.h"
#include "PinWrightHelpers.h"
#include "PinWrightSubsystem.h"
#include "Utils/ActorUtils.h"
#include "Dom/JsonObject.h"

#include "Editor.h"
#include "GameFramework/Actor.h"

// Helper to create a JSON array from a vector
static TArray<TSharedPtr<FJsonValue>> MakeVectorArray(const FVector& Vec)
{
    TArray<TSharedPtr<FJsonValue>> Arr;
    Arr.Add(MakeShared<FJsonValueNumber>(Vec.X));
    Arr.Add(MakeShared<FJsonValueNumber>(Vec.Y));
    Arr.Add(MakeShared<FJsonValueNumber>(Vec.Z));
    return Arr;
}

// ---- actor.set_transform ----
REGISTER_RPC_MUTATING_HANDLER("actor.set_transform", "actor", "Set an actor's world transform (location, rotation, scale). Each component is optional — omitted fields keep current values. Uses TeleportPhysics so physics state does not interpolate.",
    RPC_PARAMS(
        ActorNameParamUtils::ActorNameParamReq(TEXT("string"), TEXT("Display label or name of the actor. The objectPath and actorPath aliases (the key spawn/duplicate return) are also accepted." ACTORNAME_COLLISION_STEER)),
        RPC_PARAM_OPT("location", "object", "New world location as {x,y,z} in unreal units (cm); omit to leave unchanged."),
        RPC_PARAM_OPT("rotation", "object", "New world rotation as {pitch,yaw,roll} in degrees; omit to leave unchanged."),
        RPC_PARAM_OPT("scale", "object", "New 3D scale as {x,y,z} (1.0 = identity); omit to leave unchanged.")
    ))
{
  FString TargetName;
  if (!ActorNameParamUtils::RequireActorName(Ctx, TargetName)) {
    return true;
  }

  // Resolve with an explicit ambiguity verdict: a label matching several actors must not
  // be reported as "not found", and must never silently pick one.
  AActor *Found = nullptr;
  if (!ActorNameParamUtils::ResolveActorOrSendError(Ctx, nullptr, TargetName, Found)) {
    return true;
  }

  const auto& Payload = Ctx.GetRawPayload();
  FVector Location =
      ExtractVectorField(Payload, TEXT("location"), Found->GetActorLocation());
  FRotator Rotation =
      ExtractRotatorField(Payload, TEXT("rotation"), Found->GetActorRotation());
  FVector Scale =
      ExtractVectorField(Payload, TEXT("scale"), Found->GetActorScale3D());

  Found->Modify();
  Found->SetActorLocation(Location, false, nullptr,
                          ETeleportType::TeleportPhysics);
  Found->SetActorRotation(Rotation, ETeleportType::TeleportPhysics);
  Found->SetActorScale3D(Scale);
  Found->MarkComponentsRenderStateDirty();
  Found->MarkPackageDirty();

  // Verify transform
  const FVector NewLoc = Found->GetActorLocation();
  const FRotator NewRot = Found->GetActorRotation();
  const FVector NewScale = Found->GetActorScale3D();

  const bool bLocMatch = NewLoc.Equals(Location, 1.0f);
  const bool bScaleMatch = NewScale.Equals(Scale, 0.01f);

  TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
  Data->SetStringField(TEXT("actorName"), Found->GetActorLabel());
  Data->SetArrayField(TEXT("location"), MakeVectorArray(NewLoc));
  Data->SetArrayField(TEXT("scale"), MakeVectorArray(NewScale));

  if (!bLocMatch || !bScaleMatch) {
    Ctx.SendError(TEXT("TRANSFORM_MISMATCH"), TEXT("Failed to set transform exactly"));
    return true;
  }

  AddActorVerification(Data, Found);

  UE_LOG(LogPinWrightSubsystem, Display,
         TEXT("actor.set_transform: Set transform for '%s'"), *Found->GetActorLabel());
  Ctx.SendSuccess(Data);
  return true;
}

// ---- actor.get_transform ----
REGISTER_RPC_HANDLER("actor.get_transform", "actor", "Read the actor's current world transform. Returns location (cm), rotation as {pitch,yaw,roll} degrees, and 3D scale as JSON arrays.",
    RPC_PARAMS(
        ActorNameParamUtils::ActorNameParamReq(TEXT("string"), TEXT("Display label or name of the actor. The objectPath and actorPath aliases (the key spawn/duplicate return) are also accepted." ACTORNAME_COLLISION_STEER))
    ))
{
  FString TargetName;
  if (!ActorNameParamUtils::RequireActorName(Ctx, TargetName)) {
    return true;
  }

  // Resolve with an explicit ambiguity verdict: a label matching several actors must not
  // be reported as "not found", and must never silently pick one.
  AActor *Found = nullptr;
  if (!ActorNameParamUtils::ResolveActorOrSendError(Ctx, nullptr, TargetName, Found)) {
    return true;
  }

  const FTransform Current = Found->GetActorTransform();
  const FVector Location = Current.GetLocation();
  const FRotator Rotation = Current.GetRotation().Rotator();
  const FVector Scale = Current.GetScale3D();

  TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();

  Data->SetArrayField(TEXT("location"), MakeVectorArray(Location));
  TArray<TSharedPtr<FJsonValue>> RotArray;
  RotArray.Add(MakeShared<FJsonValueNumber>(Rotation.Pitch));
  RotArray.Add(MakeShared<FJsonValueNumber>(Rotation.Yaw));
  RotArray.Add(MakeShared<FJsonValueNumber>(Rotation.Roll));
  Data->SetArrayField(TEXT("rotation"), RotArray);
  Data->SetArrayField(TEXT("scale"), MakeVectorArray(Scale));

  Ctx.SendSuccess(Data);
  return true;
}

// ---- actor.get_bounding_box ----
REGISTER_RPC_HANDLER("actor.get_bounding_box", "actor", "Compute the actor's world-space axis-aligned bounding box including all primitive components (visible and hidden). Returns origin (center) and extent (half-size) as JSON arrays.",
    RPC_PARAMS(
        ActorNameParamUtils::ActorNameParamReq(TEXT("string"), TEXT("Display label or name of the actor. The objectPath and actorPath aliases (the key spawn/duplicate return) are also accepted." ACTORNAME_COLLISION_STEER))
    ))
{
  FString TargetName;
  if (!ActorNameParamUtils::RequireActorName(Ctx, TargetName)) {
    return true;
  }

  // Resolve with an explicit ambiguity verdict: a label matching several actors must not
  // be reported as "not found", and must never silently pick one.
  AActor *Found = nullptr;
  if (!ActorNameParamUtils::ResolveActorOrSendError(Ctx, nullptr, TargetName, Found)) {
    return true;
  }

  FVector Origin, BoxExtent;
  Found->GetActorBounds(false, Origin, BoxExtent);

  TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
  Data->SetArrayField(TEXT("origin"), MakeVectorArray(Origin));
  Data->SetArrayField(TEXT("extent"), MakeVectorArray(BoxExtent));
  Ctx.SendSuccess(Data);
  return true;
}
