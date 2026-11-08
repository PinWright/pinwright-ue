// Copyright (c) 2026 Alexander Penkin. MIT License.

// PhysicsHandler.cpp (runtime) - physics.setup_ragdoll / physics.activate_ragdoll on a live
// actor's skeletal mesh: the editor world in the editor, the live world in a game process.
// physics.setup_physics_simulation (creates a PhysicsAsset package) stays in the editor
// module's PhysicsHandler.cpp.

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ErrorCodes.h"
#include "Handlers/Actor/ActorNameParamUtils.h"
#include "Dispatch/PinWrightWorld.h"

#include "Animation/Skeleton.h"
#include "Components/SkeletalMeshComponent.h"
#include "Dom/JsonObject.h"
#include "EngineUtils.h"
#include "PhysicsEngine/PhysicsAsset.h"

#if WITH_EDITOR
#include "Editor.h"
#if __has_include("Subsystems/EditorActorSubsystem.h")
#include "Subsystems/EditorActorSubsystem.h"
#elif __has_include("EditorActorSubsystem.h")
#include "EditorActorSubsystem.h"
#endif
#endif

namespace
{
    // The ragdoll verbs act on the editor world in the editor (never PIE, as before the
    // runtime split) and on the live world in a game process. Sends the error on null.
    UWorld* RagdollResolveWorld(FHandlerContext& Ctx)
    {
        if (PinWright::IsGameProcess())
        {
            UWorld* World = PinWright::GetTargetWorld();
            if (!World)
            {
                Ctx.SendError(ErrorCodes::ERR_NO_WORLD, TEXT("No game world available"));
            }
            return World;
        }
        UWorld* World = PinWright::GetEditorWorldOrNull();
        if (!World)
        {
            Ctx.SendError(ErrorCodes::ERR_EDITOR_NOT_AVAILABLE, TEXT("Editor world not available"));
        }
        return World;
    }
}

// ============================================================
// physics.setup_ragdoll - Enable ragdoll physics on an actor
// (migrated from standalone HandleSetupRagdoll)
// ============================================================
REGISTER_RPC_HANDLER("physics.setup_ragdoll", "physics",
    "Enable ragdoll physics on a named actor's skeletal mesh",
    RPC_PARAMS(
        RPC_PARAM_REQ("actorName", "string", "Name or label of the target actor"),
        RPC_PARAM_OPT("blendWeight", "number", "Blend factor for animation/physics (default 1.0)"),
        RPC_PARAM_OPT("skeletonPath", "path", "Optional skeleton asset to validate")
    ))
{
  FString ActorName = Ctx.GetString("actorName");
  if (ActorName.IsEmpty()) {
    Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, TEXT("actorName required"));
    return true;
  }

  double BlendWeight = Ctx.GetNumber("blendWeight", 1.0);

  FString SkeletonPath = Ctx.GetString("skeletonPath");
  if (!SkeletonPath.IsEmpty()) {
    USkeleton *RagdollSkeleton = LoadObject<USkeleton>(nullptr, *SkeletonPath);
    if (!RagdollSkeleton) {
      Ctx.SendError(ErrorCodes::ERR_ASSET_NOT_FOUND,
                    FString::Printf(TEXT("Skeleton not found: %s"), *SkeletonPath));
      return true;
    }
  }

  UWorld *World = RagdollResolveWorld(Ctx);
  if (!World) {
    return true;
  }

  // Editor fallback list; a game process has no EditorActorSubsystem and searches the world only.
  TArray<AActor *> AllActors;
#if WITH_EDITOR
  if (!PinWright::IsGameProcess()) {
    UEditorActorSubsystem *ActorSS =
        GEditor ? GEditor->GetEditorSubsystem<UEditorActorSubsystem>() : nullptr;
    if (!ActorSS) {
      Ctx.SendError(ErrorCodes::ERR_EDITOR_ACTOR_SUBSYSTEM_MISSING,
                    TEXT("EditorActorSubsystem not available"));
      return true;
    }
    AllActors = ActorSS->GetAllLevelActors();
  }
#endif
  AActor *TargetActor = nullptr;

  for (TActorIterator<AActor> It(World); It; ++It) {
    AActor *Actor = *It;
    if (Actor) {
      if (ActorNameParamUtils::ActorLabelOrName(Actor).Equals(ActorName, ESearchCase::IgnoreCase) ||
          Actor->GetName().Equals(ActorName, ESearchCase::IgnoreCase)) {
        TargetActor = Actor;
        break;
      }
    }
  }

  if (!TargetActor) {
    for (AActor *Actor : AllActors) {
      if (Actor &&
          (ActorNameParamUtils::ActorLabelOrName(Actor).Equals(ActorName, ESearchCase::IgnoreCase) ||
           Actor->GetName().Equals(ActorName, ESearchCase::IgnoreCase))) {
        TargetActor = Actor;
        break;
      }
    }
  }

  if (!TargetActor) {
    Ctx.SendError(ErrorCodes::ERR_ACTOR_NOT_FOUND,
                  FString::Printf(TEXT("Actor not found: %s"), *ActorName));
    return true;
  }

  USkeletalMeshComponent *SkelMeshComp =
      TargetActor->FindComponentByClass<USkeletalMeshComponent>();
  if (!SkelMeshComp) {
    Ctx.SendError(ErrorCodes::ERR_COMPONENT_NOT_FOUND,
                  TEXT("Skeletal mesh component not found"));
    return true;
  }

  SkelMeshComp->SetSimulatePhysics(true);
  SkelMeshComp->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);

  if (SkelMeshComp->GetPhysicsAsset()) {
    SkelMeshComp->SetAllBodiesSimulatePhysics(true);
    SkelMeshComp->SetUpdateAnimationInEditor(BlendWeight < 1.0);
  }

  TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
  Resp->SetStringField(TEXT("actorName"), ActorName);
  Resp->SetNumberField(TEXT("blendWeight"), BlendWeight);
  Resp->SetBoolField(TEXT("ragdollActive"),
                     SkelMeshComp->IsSimulatingPhysics());
  Resp->SetBoolField(TEXT("hasPhysicsAsset"),
                     SkelMeshComp->GetPhysicsAsset() != nullptr);

  if (SkelMeshComp->GetPhysicsAsset()) {
    Resp->SetStringField(TEXT("physicsAssetPath"),
                         SkelMeshComp->GetPhysicsAsset()->GetPathName());
  }

  Ctx.SendSuccess(Resp);
  return true;
}

// ============================================================
// physics.activate_ragdoll - Toggle ragdoll activation on an actor
// (migrated from standalone HandleActivateRagdoll)
// ============================================================
REGISTER_RPC_HANDLER("physics.activate_ragdoll", "physics",
    "Activate or deactivate ragdoll physics on a named actor",
    RPC_PARAMS(
        RPC_PARAM_REQ("actorName", "string", "Name or label of the target actor"),
        RPC_PARAM_OPT("activate", "boolean", "true to activate, false to deactivate (default true)")
    ))
{
  FString ActorName = Ctx.GetString("actorName");
  if (ActorName.IsEmpty()) {
    Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, TEXT("actorName required"));
    return true;
  }

  bool bActivate = Ctx.GetBool("activate", true);

  UWorld *World = RagdollResolveWorld(Ctx);
  if (!World) {
    return true;
  }
  AActor *TargetActor = nullptr;

  for (TActorIterator<AActor> It(World); It; ++It) {
    AActor *Actor = *It;
    if (Actor) {
      if (ActorNameParamUtils::ActorLabelOrName(Actor).Equals(ActorName, ESearchCase::IgnoreCase) ||
          Actor->GetName().Equals(ActorName, ESearchCase::IgnoreCase)) {
        TargetActor = Actor;
        break;
      }
    }
  }

  if (!TargetActor) {
    Ctx.SendError(ErrorCodes::ERR_ACTOR_NOT_FOUND,
                  FString::Printf(TEXT("Actor not found: %s"), *ActorName));
    return true;
  }

  USkeletalMeshComponent *SkelMeshComp =
      TargetActor->FindComponentByClass<USkeletalMeshComponent>();
  if (!SkelMeshComp) {
    Ctx.SendError(ErrorCodes::ERR_COMPONENT_NOT_FOUND,
                  TEXT("Skeletal mesh component not found"));
    return true;
  }

  // Activate or deactivate ragdoll
  if (bActivate) {
    SkelMeshComp->SetSimulatePhysics(true);
    SkelMeshComp->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
    if (SkelMeshComp->GetPhysicsAsset()) {
      SkelMeshComp->SetAllBodiesSimulatePhysics(true);
    }
  } else {
    SkelMeshComp->SetAllBodiesSimulatePhysics(false);
    SkelMeshComp->SetSimulatePhysics(false);
    SkelMeshComp->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
  }

  TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
  Resp->SetStringField(TEXT("actorName"), ActorName);
  Resp->SetBoolField(TEXT("activate"), bActivate);
  Resp->SetBoolField(TEXT("ragdollActive"),
                     SkelMeshComp->IsSimulatingPhysics());
  Resp->SetBoolField(TEXT("hasPhysicsAsset"),
                     SkelMeshComp->GetPhysicsAsset() != nullptr);

  if (SkelMeshComp->GetPhysicsAsset()) {
    Resp->SetStringField(TEXT("physicsAssetPath"),
                         SkelMeshComp->GetPhysicsAsset()->GetPathName());
  }

  Ctx.SendSuccess(Resp);
  return true;
}
