// Copyright (c) 2026 Alexander Penkin. MIT License.

// ActorNudgeHandler.cpp - actor.nudge
// Relative move/rotate: translate an actor by a world-space or camera-relative delta
// and optionally add a rotation delta, then return the resulting transform.

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ParamSpec.h"
#include "Handlers/Actor/ActorNameParamUtils.h"
#include "Handlers/Actor/ActorBatchUtils.h"
#include "Handlers/ErrorCodes.h"
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

  // Top-level keys that belong inside each actors[] entry in batch form.
  const TArray<FString>& NudgeSingleFormKeys()
  {
    static const TArray<FString> Keys = [] {
      TArray<FString> K = ActorNameParamUtils::ActorNameKeys();
      K.Append({TEXT("deltaWorld"), TEXT("deltaCamera"), TEXT("deltaRotation")});
      return K;
    }();
    return Keys;
  }

  bool NudgeHasAnyDelta(const TSharedPtr<FJsonObject>& Params)
  {
    return Params.IsValid() && (Params->HasField(TEXT("deltaWorld")) ||
        Params->HasField(TEXT("deltaCamera")) || Params->HasField(TEXT("deltaRotation")));
  }

  // Applies one request (the payload, or one actors[] entry) to a resolved actor. This is the
  // verb's original single-actor body, so its success payload and errors are unchanged.
  ActorBatchUtils::FEntryOutcome NudgeApply(AActor* Found, const TSharedPtr<FJsonObject>& Params)
  {
    const bool bHasWorld = Params.IsValid() && Params->HasField(TEXT("deltaWorld"));
    const bool bHasCamera = Params.IsValid() && Params->HasField(TEXT("deltaCamera"));
    const bool bHasRotation = Params.IsValid() && Params->HasField(TEXT("deltaRotation"));

    if (!bHasWorld && !bHasCamera && !bHasRotation)
    {
      return ActorBatchUtils::Fail(ErrorCodes::ERR_INVALID_PARAMS,
          TEXT("Provide at least one of deltaWorld, deltaCamera, or deltaRotation."));
    }

    // Resolve the world-space translation delta. deltaWorld wins if both are provided.
    FVector WorldDelta = FVector::ZeroVector;
    if (bHasWorld)
    {
      WorldDelta = ExtractVectorField(Params, TEXT("deltaWorld"), FVector::ZeroVector);
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
        return ActorBatchUtils::Fail(ErrorCodes::ERR_VIEWPORT_NOT_AVAILABLE,
            TEXT("deltaCamera needs an active editor viewport; none is available. Use deltaWorld instead."));
      }

      const FRotator ViewRot = ViewportClient->GetViewRotation();
      const FVector Forward = ViewRot.Vector();
      const FVector Right = FRotationMatrix(ViewRot).GetScaledAxis(EAxis::Y);
      const FVector Up = FRotationMatrix(ViewRot).GetScaledAxis(EAxis::Z);

      double R = 0.0, U = 0.0, F = 0.0;
      const TSharedPtr<FJsonObject>* Cam = nullptr;
      if (Params->TryGetObjectField(TEXT("deltaCamera"), Cam) && Cam && Cam->IsValid())
      {
        (*Cam)->TryGetNumberField(TEXT("right"), R);
        (*Cam)->TryGetNumberField(TEXT("up"), U);
        (*Cam)->TryGetNumberField(TEXT("forward"), F);
      }
      WorldDelta = Right * R + Up * U + Forward * F;
    }

    const FRotator DeltaRotation = bHasRotation
        ? ExtractRotatorField(Params, TEXT("deltaRotation"), FRotator::ZeroRotator)
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
    return ActorBatchUtils::Succeed(Data);
  }
}

// ---- actor.nudge ----
REGISTER_RPC_HANDLER("actor.nudge", "actor", "Move and/or rotate an actor by a RELATIVE delta (not an absolute transform). Translate by deltaWorld {x,y,z} along world axes, or by deltaCamera {right,up,forward} in the active viewport camera's basis (for 'push it away from me' moves); optionally add deltaRotation {pitch,yaw,roll} degrees. At least one delta is required. Returns the resulting transform. Pass actors[] instead of actorName to nudge several actors in one call under one undo transaction, with per-entry results.",
    RPC_PARAMS(
        ActorNameParamUtils::ActorNameParamOpt(TEXT("string"), TEXT("Display label or name of the actor to nudge; required unless actors is given. The objectPath and actorPath aliases are also accepted." ACTORNAME_COLLISION_STEER)),
        RPC_PARAM_OPT("deltaWorld", "object", "World-space translation delta as {x,y,z} in unreal units (cm), added to the current location."),
        RPC_PARAM_OPT("deltaCamera", "object", "Camera-relative translation delta as {right,up,forward} in cm, resolved against the active viewport camera basis and added to the current location. Requires an active editor viewport."),
        RPC_PARAM_OPT("deltaRotation", "object", "Rotation delta as {pitch,yaw,roll} in degrees, added to the current rotation."),
        RPC_PARAM_OPT_NESTED("actors", "array", "Batch form, replacing actorName and the top-level deltas: entry objects {actorName, objectPath, actorPath, actor_name, deltaWorld, deltaCamera, deltaRotation}, each naming one actor (actorName or an alias) plus at least one delta with the single-form meaning. These keys are the whole entry schema and any other key inside an entry is refused with UNKNOWN_NESTED_PARAMS. Applied in order under one undo transaction; a malformed entry refuses the whole batch before anything moves, while an unresolved actor or failed entry is reported per entry and does not undo the others.",
            TEXT("actorName"), TEXT("objectPath"), TEXT("actorPath"), TEXT("actor_name"), TEXT("deltaWorld"),
            TEXT("deltaCamera"), TEXT("deltaRotation"))
    ))
{
  const auto& Payload = Ctx.GetRawPayload();

  if (Payload.IsValid() && Payload->HasField(TEXT("actors")))
  {
    TArray<TSharedPtr<FJsonObject>> Entries;
    if (!ActorBatchUtils::ReadEntries(Ctx, NudgeSingleFormKeys(),
            [](const TSharedPtr<FJsonObject>& Entry, FString& OutError)
            {
              if (NudgeHasAnyDelta(Entry))
              {
                return true;
              }
              OutError = TEXT("provide at least one of deltaWorld, deltaCamera, or deltaRotation");
              return false;
            },
            Entries))
    {
      return true;
    }
    ActorBatchUtils::RunBatch(Ctx, TEXT("MCP: actor.nudge"), Entries,
        [](AActor* Actor, const TSharedPtr<FJsonObject>& Entry) { return NudgeApply(Actor, Entry); });
    return true;
  }

  const FString TargetName = ActorNameParamUtils::ResolveActorName(Ctx);
  if (TargetName.IsEmpty())
  {
    Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
        TEXT("Provide actorName (aliases objectPath, actorPath, actor_name) for one actor, or actors[] for a batch."));
    return true;
  }

  // Resolve with an explicit ambiguity verdict: a label matching several actors must not
  // be reported as "not found", and must never silently pick one.
  AActor* Found = nullptr;
  if (!ActorNameParamUtils::ResolveActorOrSendError(Ctx, nullptr, TargetName, Found))
  {
    return true;
  }

  ActorBatchUtils::SendOutcome(Ctx, NudgeApply(Found, Payload));
  return true;
}
