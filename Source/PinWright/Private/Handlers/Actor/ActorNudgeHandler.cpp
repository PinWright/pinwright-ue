// Copyright (c) 2026 Alexander Penkin. MIT License.

// ActorNudgeHandler.cpp - actor.nudge
// Relative move/rotate: translate an actor by a world-space or camera-relative delta
// and optionally add a rotation delta, then return the resulting transform.

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ParamSpec.h"
#include "Handlers/Actor/ActorNameParamUtils.h"
#include "Handlers/Editor/EditorHandlerUtils.h"
#include "PinWrightGlobals.h"
#include "PinWrightHelpers.h"
#include "PinWrightSubsystem.h"
#include "Utils/ActorUtils.h"
#include "Dom/JsonObject.h"

#include "Editor.h"
#include "EditorViewportClient.h"
#include "Math/RotationMatrix.h"
#include "GameFramework/Actor.h"

namespace
{
  // File-local (uniquely named to avoid an ODR clash with ActorTransformHandler's
  // MakeVectorArray under Unity) helper building a [x,y,z] JSON number array.
  TArray<TSharedPtr<FJsonValue>> NudgeVectorToArray(const FVector& V)
  {
    TArray<TSharedPtr<FJsonValue>> Arr;
    Arr.Add(MakeShared<FJsonValueNumber>(V.X));
    Arr.Add(MakeShared<FJsonValueNumber>(V.Y));
    Arr.Add(MakeShared<FJsonValueNumber>(V.Z));
    return Arr;
  }
}

// ---- actor.nudge ----
REGISTER_RPC_HANDLER("actor.nudge", "actor", "Move and/or rotate an actor by a RELATIVE delta (not an absolute transform). Translate by deltaWorld {x,y,z} along world axes, or by deltaCamera {right,up,forward} in the active viewport camera's basis (for 'push it away from me' moves); optionally add deltaRotation {pitch,yaw,roll} degrees. At least one delta is required. Returns the resulting transform.",
    RPC_PARAMS(
        ActorNameParamUtils::ActorNameParamReq(TEXT("string"), TEXT("Display label or name of the actor to nudge. The objectPath and actorPath aliases are also accepted." ACTORNAME_COLLISION_STEER)),
        RPC_PARAM_OPT("deltaWorld", "object", "World-space translation delta as {x,y,z} in unreal units (cm), added to the current location."),
        RPC_PARAM_OPT("deltaCamera", "object", "Camera-relative translation delta as {right,up,forward} in cm, resolved against the active viewport camera basis and added to the current location. Requires an active editor viewport."),
        RPC_PARAM_OPT("deltaRotation", "object", "Rotation delta as {pitch,yaw,roll} in degrees, added to the current rotation.")
    ))
{
  FString TargetName;
  if (!ActorNameParamUtils::RequireActorName(Ctx, TargetName))
  {
    return true;
  }

  // Resolve with an explicit ambiguity verdict: a label matching several actors must not
  // be reported as "not found", and must never silently pick one.
  AActor* Found = nullptr;
  if (!ActorNameParamUtils::ResolveActorOrSendError(Ctx, nullptr, TargetName, Found))
  {
    return true;
  }

  const auto& Payload = Ctx.GetRawPayload();
  const bool bHasWorld = Payload.IsValid() && Payload->HasField(TEXT("deltaWorld"));
  const bool bHasCamera = Payload.IsValid() && Payload->HasField(TEXT("deltaCamera"));
  const bool bHasRotation = Payload.IsValid() && Payload->HasField(TEXT("deltaRotation"));

  if (!bHasWorld && !bHasCamera && !bHasRotation)
  {
    Ctx.SendError(TEXT("INVALID_PARAMS"),
        TEXT("Provide at least one of deltaWorld, deltaCamera, or deltaRotation."));
    return true;
  }

  // Resolve the world-space translation delta. deltaWorld wins if both are provided.
  FVector WorldDelta = FVector::ZeroVector;
  if (bHasWorld)
  {
    WorldDelta = Ctx.GetVector(TEXT("deltaWorld"), FVector::ZeroVector);
  }
  else if (bHasCamera)
  {
    // The shared typed resolver, not GetActiveViewport()->GetClient(): under PIE the active
    // viewport's client is the game client, and reading a camera through it as an
    // FEditorViewportClient is undefined behaviour (see EditorHandlerUtils.h). This route also
    // keeps deltaCamera working while PIE runs, against the editor camera the nudge is relative to.
    FEditorViewportClient* ViewportClient =
        EditorHandlerUtils::ResolveActiveLevelViewportClient();
    if (!ViewportClient)
    {
      Ctx.SendError(TEXT("VIEWPORT_NOT_AVAILABLE"),
          TEXT("deltaCamera needs an active editor viewport; none is available. Use deltaWorld instead."));
      return true;
    }

    const FRotator ViewRot = ViewportClient->GetViewRotation();
    const FVector Forward = ViewRot.Vector();
    const FVector Right = FRotationMatrix(ViewRot).GetScaledAxis(EAxis::Y);
    const FVector Up = FRotationMatrix(ViewRot).GetScaledAxis(EAxis::Z);

    double R = 0.0, U = 0.0, F = 0.0;
    if (TSharedPtr<FJsonObject> Cam = Ctx.GetObject(TEXT("deltaCamera")))
    {
      Cam->TryGetNumberField(TEXT("right"), R);
      Cam->TryGetNumberField(TEXT("up"), U);
      Cam->TryGetNumberField(TEXT("forward"), F);
    }
    WorldDelta = Right * R + Up * U + Forward * F;
  }

  const FRotator DeltaRotation = bHasRotation
      ? Ctx.GetRotator(TEXT("deltaRotation"), FRotator::ZeroRotator)
      : FRotator::ZeroRotator;

  const FVector NewLocation = Found->GetActorLocation() + WorldDelta;
  const FRotator NewRotation = Found->GetActorRotation() + DeltaRotation;

  Found->Modify();
  Found->SetActorLocation(NewLocation, false, nullptr, ETeleportType::TeleportPhysics);
  Found->SetActorRotation(NewRotation, ETeleportType::TeleportPhysics);
  Found->MarkComponentsRenderStateDirty();
  Found->MarkPackageDirty();

  const FVector AppliedLoc = Found->GetActorLocation();
  const FRotator AppliedRot = Found->GetActorRotation();
  const FVector AppliedScale = Found->GetActorScale3D();

  FVector BoundsOrigin, BoundsExtent;
  Found->GetActorBounds(false, BoundsOrigin, BoundsExtent);

  TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
  Data->SetStringField(TEXT("actorName"), Found->GetActorLabel());
  Data->SetArrayField(TEXT("location"), NudgeVectorToArray(AppliedLoc));

  TArray<TSharedPtr<FJsonValue>> RotArr;
  RotArr.Add(MakeShared<FJsonValueNumber>(AppliedRot.Pitch));
  RotArr.Add(MakeShared<FJsonValueNumber>(AppliedRot.Yaw));
  RotArr.Add(MakeShared<FJsonValueNumber>(AppliedRot.Roll));
  Data->SetArrayField(TEXT("rotation"), RotArr);

  Data->SetArrayField(TEXT("scale"), NudgeVectorToArray(AppliedScale));
  Data->SetArrayField(TEXT("appliedDelta"), NudgeVectorToArray(WorldDelta));
  Data->SetStringField(TEXT("units"), TEXT("cm"));
  Data->SetStringField(TEXT("axis"),
      TEXT("Unreal left-handed, Z-up: +X forward, +Y right, +Z up; rotation in degrees"));

  // pivot = the actor's transform origin (its location) about which the move applied.
  Data->SetArrayField(TEXT("pivot"), NudgeVectorToArray(AppliedLoc));

  TSharedPtr<FJsonObject> Bounds = MakeShared<FJsonObject>();
  Bounds->SetArrayField(TEXT("origin"), NudgeVectorToArray(BoundsOrigin));
  Bounds->SetArrayField(TEXT("extent"), NudgeVectorToArray(BoundsExtent));
  Data->SetObjectField(TEXT("bounds"), Bounds);

  AddActorVerification(Data, Found);

  UE_LOG(LogPinWrightSubsystem, Display,
         TEXT("actor.nudge: Nudged '%s' by (%f, %f, %f)"),
         *Found->GetActorLabel(), WorldDelta.X, WorldDelta.Y, WorldDelta.Z);
  Ctx.SendSuccess(Data);
  return true;
}
