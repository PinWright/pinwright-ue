// Copyright (c) 2026 Alexander Penkin. MIT License.

// ActorTransformHandler.cpp - Migrated from PinWright_ControlHandlers.cpp
// Handles actor.set_transform, actor.get_transform, actor.get_bounding_box

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ParamSpec.h"
#include "Handlers/Actor/ActorNameParamUtils.h"
#include "Handlers/Actor/ActorBatchUtils.h"
#include "Handlers/ErrorCodes.h"
#include "PinWrightGlobals.h"
#include "PinWrightHelpers.h"
#include "PinWrightSubsystem.h"
#include "Utils/ActorUtils.h"
#include "Dom/JsonObject.h"

#include "Editor.h"
#include "GameFramework/Actor.h"
#include "Math/RotationMatrix.h"

// Helper to create a JSON array from a vector
static TArray<TSharedPtr<FJsonValue>> MakeVectorArray(const FVector& Vec)
{
    TArray<TSharedPtr<FJsonValue>> Arr;
    Arr.Add(MakeShared<FJsonValueNumber>(Vec.X));
    Arr.Add(MakeShared<FJsonValueNumber>(Vec.Y));
    Arr.Add(MakeShared<FJsonValueNumber>(Vec.Z));
    return Arr;
}

namespace
{
  // lookAt/roll of one request (the payload, or one actors[] entry). Names are unique to
  // this file because Unity merges sibling handler TUs.
  struct FSetTransformLookAt
  {
    bool bSet = false;
    FString TargetActor; // non-empty: aim at this actor's location, else at TargetPoint
    FVector TargetPoint = FVector::ZeroVector;
    double Roll = 0.0;
  };

  // Top-level keys that belong inside each actors[] entry in batch form.
  const TArray<FString>& SetTransformSingleFormKeys()
  {
    static const TArray<FString> Keys = [] {
      TArray<FString> K = ActorNameParamUtils::ActorNameKeys();
      K.Append({TEXT("location"), TEXT("rotation"), TEXT("scale"), TEXT("lookAt"), TEXT("roll")});
      return K;
    }();
    return Keys;
  }

  // Validates and reads lookAt/roll without touching the world.
  bool SetTransformParseLookAt(const TSharedPtr<FJsonObject>& Params, FSetTransformLookAt& Out, FString& OutError)
  {
    Out = FSetTransformLookAt();
    if (!Params.IsValid()) {
      return true;
    }
    const bool bHasRoll = Params->HasField(TEXT("roll"));
    const TSharedPtr<FJsonValue> LookAt = Params->TryGetField(TEXT("lookAt"));
    if (!LookAt.IsValid()) {
      if (bHasRoll) {
        OutError = TEXT("roll only applies together with lookAt; use rotation {pitch,yaw,roll} otherwise.");
        return false;
      }
      return true;
    }
    if (Params->HasField(TEXT("rotation"))) {
      OutError = TEXT("lookAt and rotation are mutually exclusive: lookAt derives the rotation (pass roll for the twist about the aim axis).");
      return false;
    }
    if (bHasRoll && !Params->TryGetNumberField(TEXT("roll"), Out.Roll)) {
      OutError = TEXT("roll must be a number of degrees.");
      return false;
    }
    if (LookAt->Type == EJson::String) {
      Out.TargetActor = LookAt->AsString();
      if (Out.TargetActor.IsEmpty()) {
        OutError = TEXT("lookAt is an empty actor identifier.");
        return false;
      }
    } else if (LookAt->Type == EJson::Object) {
      const TSharedPtr<FJsonObject> Point = LookAt->AsObject();
      double X = 0.0, Y = 0.0, Z = 0.0;
      if (!Point.IsValid() || Point->Values.Num() != 3 || !Point->TryGetNumberField(TEXT("x"), X) ||
          !Point->TryGetNumberField(TEXT("y"), Y) || !Point->TryGetNumberField(TEXT("z"), Z)) {
        OutError = TEXT("lookAt point must be exactly {x,y,z} numbers in cm.");
        return false;
      }
      Out.TargetPoint = FVector(X, Y, Z);
    } else {
      OutError = TEXT("lookAt must be a world point {x,y,z} or an actor identifier string.");
      return false;
    }
    Out.bSet = true;
    return true;
  }

  // Applies one validated request to a resolved actor. The no-lookAt path is the verb's
  // original single-actor body, so its success payload and TRANSFORM_MISMATCH are unchanged.
  ActorBatchUtils::FEntryOutcome SetTransformApply(AActor* Found, const TSharedPtr<FJsonObject>& Params,
                                                   const FSetTransformLookAt& LookAt)
  {
    FVector Location =
        ExtractVectorField(Params, TEXT("location"), Found->GetActorLocation());
    FRotator Rotation =
        ExtractRotatorField(Params, TEXT("rotation"), Found->GetActorRotation());
    FVector Scale =
        ExtractVectorField(Params, TEXT("scale"), Found->GetActorScale3D());

    AActor* TargetActor = nullptr;
    FVector AimTarget = LookAt.TargetPoint;
    FVector AimDir = FVector::ZeroVector;
    if (LookAt.bSet) {
      if (!LookAt.TargetActor.IsEmpty()) {
        const McpActorUtils::FActorResolution Resolution =
            McpActorUtils::ResolveActor(nullptr, LookAt.TargetActor);
        if (!Resolution.IsResolved()) {
          return ActorBatchUtils::ResolutionFailure(LookAt.TargetActor, Resolution, TEXT("lookAt target"));
        }
        TargetActor = Resolution.Actor;
        if (TargetActor == Found) {
          return ActorBatchUtils::Fail(ErrorCodes::ERR_INVALID_ARGUMENT,
              TEXT("lookAt names the actor being transformed; an actor cannot aim at itself."));
        }
        AimTarget = TargetActor->GetActorLocation();
      }
      // Aim from the location the actor will have after this call, not the current one.
      AimDir = AimTarget - Location;
      if (AimDir.IsNearlyZero(UE_KINDA_SMALL_NUMBER)) {
        return ActorBatchUtils::Fail(ErrorCodes::ERR_INVALID_ARGUMENT,
            TEXT("lookAt target coincides with the actor's location, so there is no direction to aim along."));
      }
      AimDir.Normalize();
      Rotation = FRotationMatrix::MakeFromX(AimDir).Rotator();
      Rotation.Roll = LookAt.Roll;
    }

    Found->Modify();
    Found->SetActorLocation(Location, false, nullptr,
                            ETeleportType::TeleportPhysics);
    Found->SetActorRotation(Rotation, ETeleportType::TeleportPhysics);
    Found->SetActorScale3D(Scale);
    Found->MarkComponentsRenderStateDirty();
    Found->MarkPackageDirty();

    // Verify transform
    const FVector NewLoc = Found->GetActorLocation();
    const FVector NewScale = Found->GetActorScale3D();

    const bool bLocMatch = NewLoc.Equals(Location, 1.0f);
    const bool bScaleMatch = NewScale.Equals(Scale, 0.01f);

    // lookAt is verified on the actor's measured forward axis, not on the requested rotator.
    double AimErrorDegrees = 0.0;
    if (LookAt.bSet) {
      const double Cos = FMath::Clamp(FVector::DotProduct(Found->GetActorForwardVector(), AimDir), -1.0, 1.0);
      AimErrorDegrees = FMath::RadiansToDegrees(FMath::Acos(Cos));
    }
    const bool bAimMatch = AimErrorDegrees <= 0.1;

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("actorName"), Found->GetActorLabel());
    Data->SetArrayField(TEXT("location"), MakeVectorArray(NewLoc));
    Data->SetArrayField(TEXT("scale"), MakeVectorArray(NewScale));

    if (!bLocMatch || !bScaleMatch || !bAimMatch) {
      return ActorBatchUtils::Fail(ErrorCodes::ERR_TRANSFORM_MISMATCH, TEXT("Failed to set transform exactly"));
    }

    if (LookAt.bSet) {
      const FRotator NewRot = Found->GetActorRotation();
      TSharedPtr<FJsonObject> Aim = MakeShared<FJsonObject>();
      Aim->SetArrayField(TEXT("target"), MakeVectorArray(AimTarget));
      if (TargetActor) {
        Aim->SetStringField(TEXT("targetActor"), TargetActor->GetActorLabel());
        Aim->SetStringField(TEXT("targetActorObjectName"), TargetActor->GetName());
      }
      TArray<TSharedPtr<FJsonValue>> RotArr;
      RotArr.Add(MakeShared<FJsonValueNumber>(NewRot.Pitch));
      RotArr.Add(MakeShared<FJsonValueNumber>(NewRot.Yaw));
      RotArr.Add(MakeShared<FJsonValueNumber>(NewRot.Roll));
      Aim->SetArrayField(TEXT("rotation"), RotArr);
      Aim->SetNumberField(TEXT("roll"), LookAt.Roll);
      Aim->SetNumberField(TEXT("aimErrorDegrees"), AimErrorDegrees);
      Data->SetObjectField(TEXT("lookAt"), Aim);
    }

    AddActorVerification(Data, Found);

    UE_LOG(LogPinWrightSubsystem, Display,
           TEXT("actor.set_transform: Set transform for '%s'"), *Found->GetActorLabel());
    return ActorBatchUtils::Succeed(Data);
  }
}

// ---- actor.set_transform ----
REGISTER_RPC_MUTATING_HANDLER("actor.set_transform", "actor", "Set an actor's world transform (location, rotation, scale), optionally aiming it with lookAt. Each component is optional — omitted fields keep current values. Uses TeleportPhysics so physics state does not interpolate. Pass actors[] instead of actorName to transform several actors in one call under one undo transaction, with per-entry results.",
    RPC_PARAMS(
        ActorNameParamUtils::ActorNameParamOpt(TEXT("string"), TEXT("Display label or name of the actor; required unless actors is given. The objectPath and actorPath aliases (the key spawn/duplicate return) are also accepted." ACTORNAME_COLLISION_STEER)),
        RPC_PARAM_OPT("location", "object", "New world location as {x,y,z} in unreal units (cm); omit to leave unchanged."),
        RPC_PARAM_OPT("rotation", "object", "New world rotation as {pitch,yaw,roll} in degrees; omit to leave unchanged. Mutually exclusive with lookAt."),
        RPC_PARAM_OPT("scale", "object", "New 3D scale as {x,y,z} (1.0 = identity); omit to leave unchanged."),
        RPC_PARAM_OPT("lookAt", "object|string", "Aim the actor's forward (+X) axis at a world point {x,y,z} in cm, or at another actor's location (string: label, internal object name or object path). Derives the rotation from the new location, so it cannot be combined with rotation. The response adds a lookAt block with the measured aimErrorDegrees."),
        RPC_PARAM_OPT("roll", "number", "Twist in degrees about the aim axis; only valid with lookAt. Default 0."),
        RPC_PARAM_OPT_NESTED("actors", "array", "Batch form, replacing actorName and the top-level transform fields: entry objects {actorName, objectPath, actorPath, actor_name, location, rotation, scale, lookAt, roll}, each naming one actor (actorName or an alias) plus the same optional fields as the single form. These keys are the whole entry schema and any other key inside an entry is refused with UNKNOWN_NESTED_PARAMS. Applied in order under one undo transaction; a malformed entry refuses the whole batch before anything moves, while an unresolved actor or failed entry is reported per entry and does not undo the others.",
            TEXT("actorName"), TEXT("objectPath"), TEXT("actorPath"), TEXT("actor_name"), TEXT("location"),
            TEXT("rotation"), TEXT("scale"), TEXT("lookAt"), TEXT("roll"))
    ))
{
  const auto& Payload = Ctx.GetRawPayload();

  if (Payload.IsValid() && Payload->HasField(TEXT("actors"))) {
    TArray<TSharedPtr<FJsonObject>> Entries;
    if (!ActorBatchUtils::ReadEntries(Ctx, SetTransformSingleFormKeys(),
            [](const TSharedPtr<FJsonObject>& Entry, FString& OutError) {
              FSetTransformLookAt Unused;
              return SetTransformParseLookAt(Entry, Unused, OutError);
            },
            Entries)) {
      return true;
    }
    ActorBatchUtils::RunBatch(Ctx, TEXT("MCP: actor.set_transform"), Entries,
        [](AActor* Actor, const TSharedPtr<FJsonObject>& Entry) {
          FSetTransformLookAt LookAt;
          FString Unused;
          SetTransformParseLookAt(Entry, LookAt, Unused); // already validated by ReadEntries
          return SetTransformApply(Actor, Entry, LookAt);
        });
    return true;
  }

  const FString TargetName = ActorNameParamUtils::ResolveActorName(Ctx);
  if (TargetName.IsEmpty()) {
    Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
        TEXT("Provide actorName (aliases objectPath, actorPath, actor_name) for one actor, or actors[] for a batch."));
    return true;
  }

  FSetTransformLookAt LookAt;
  FString LookAtError;
  if (!SetTransformParseLookAt(Payload, LookAt, LookAtError)) {
    Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, LookAtError);
    return true;
  }

  // Resolve with an explicit ambiguity verdict: a label matching several actors must not
  // be reported as "not found", and must never silently pick one.
  AActor *Found = nullptr;
  if (!ActorNameParamUtils::ResolveActorOrSendError(Ctx, nullptr, TargetName, Found)) {
    return true;
  }

  ActorBatchUtils::SendOutcome(Ctx, SetTransformApply(Found, Payload, LookAt));
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
