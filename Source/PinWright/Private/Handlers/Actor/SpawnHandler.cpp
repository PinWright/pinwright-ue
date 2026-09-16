// Copyright (c) 2026 Alexander Penkin. MIT License.

// SpawnHandler.cpp - Migrated from PinWright_ControlHandlers.cpp
// Handles actor.spawn and actor.spawn_from_blueprint

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ParamSpec.h"
#include "Handlers/Actor/SpawnMaterialUtils.h"
#include "Handlers/Actor/SpawnParamUtils.h"
#include "Handlers/Volume/VolumeBrushGeometry.h"
#include "PinWrightGlobals.h"
#include "PinWrightHelpers.h"
#include "PinWrightSubsystem.h"
#include "Utils/ActorUtils.h"
#include "Utils/AssetUtils.h"
#include "Dom/JsonObject.h"

#include "Editor.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/Blueprint.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Animation/SkeletalMeshActor.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Volume.h"

namespace
{
  static UWorld* ResolveSpawnWorld()
  {
    if (!GEditor) {
      return nullptr;
    }
    if (GEditor->PlayWorld) {
      return GEditor->PlayWorld;
    }
    return GEditor->GetEditorWorldContext().World();
  }

  static AActor* SpawnActorInWorld(UWorld* World, UClass* ClassToSpawn,
                                   const FVector& Location,
                                   const FRotator& Rotation)
  {
    if (!World || !ClassToSpawn) {
      return nullptr;
    }

    FActorSpawnParameters SpawnParams;
    SpawnParams.SpawnCollisionHandlingOverride =
        ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButAlwaysSpawn;
    return World->SpawnActor(ClassToSpawn, &Location, &Rotation, SpawnParams);
  }
}

// ---- actor.spawn ----
REGISTER_RPC_HANDLER("actor.spawn", "actor", "Spawn an actor in the editor world. Either classPath or meshPath is required: classPath accepts UClass names, /Script paths, BP asset paths, or static/skeletal mesh paths (which auto-pick StaticMeshActor/SkeletalMeshActor). For Blueprint-only spawns prefer actor.spawn_from_blueprint.",
    RPC_PARAMS(
        SpawnParamUtils::SpawnClassPathParamOpt(TEXT("Class or asset path to spawn: UClass name (e.g. 'StaticMeshActor'), /Script/Engine.X path, /Game/.../BP_X BP asset path, or a StaticMesh/SkeletalMesh asset path (which auto-picks StaticMeshActor/SkeletalMeshActor). This is the slot the surface-wide assetPath alias resolves to.")),
        SpawnParamUtils::SpawnLabelParamOpt(TEXT("Display label for the spawned actor; auto-derived from mesh/class name when omitted.")),
        SpawnParamUtils::SpawnMeshPathParamOpt(TEXT("Static or skeletal mesh asset path (e.g. /Game/Meshes/SM_Cube). Forces StaticMeshActor/SkeletalMeshActor as the spawn class. Prefer classPath/assetPath, which resolves a mesh path identically and a class path as well.")),
        RPC_PARAM_OPT("location", "object", "World-space location as {x,y,z} in unreal units (cm); defaults to origin."),
        RPC_PARAM_OPT("rotation", "object", "World-space rotation as {pitch,yaw,roll} in degrees; defaults to identity."),
        RPC_PARAM_OPT("scale", "object", "World-space 3D scale as {x,y,z} (1.0 = identity); defaults to (1,1,1). Spawn scaled directly instead of a follow-up actor.set_transform."),
        SpawnMaterialUtils::MaterialPathParam(),
        SpawnMaterialUtils::MaterialPathsParam()
    ))
{
  auto* Subsystem = Ctx.GetSubsystem();
  const auto& Payload = Ctx.GetRawPayload();

  FString ClassPath = SpawnParamUtils::ResolveSpawnClassPath(Ctx);
  FString ActorName = SpawnParamUtils::ResolveSpawnLabel(Ctx);
  FVector Location = Ctx.GetVector(TEXT("location"), FVector::ZeroVector);
  FRotator Rotation = Ctx.GetRotator(TEXT("rotation"), FRotator::ZeroRotator);
  FVector Scale = Ctx.GetVector(TEXT("scale"), FVector::OneVector);

  UClass *ResolvedClass = nullptr;
  FString MeshPath = SpawnParamUtils::ResolveSpawnMeshPath(Ctx);
  UStaticMesh *ResolvedStaticMesh = nullptr;
  USkeletalMesh *ResolvedSkeletalMesh = nullptr;

  if (ClassPath.IsEmpty() && MeshPath.IsEmpty()) {
    Ctx.SendError(TEXT("INVALID_PARAMS"),
        TEXT("Either classPath (aliases: assetPath, class_name, className) or meshPath (alias: mesh_path) is required. If using the MCP CLI, avoid mixing key=value arguments with --params-json."));
    return true;
  }

  // Resolve materialPath / materialPaths BEFORE spawning: a typo'd material path is a
  // caller mistake we can reject at zero cost, and failing after SpawnActor would strand
  // an orphan actor in the level. See SpawnMaterialUtils.h for why this is stricter than
  // blueprint.scs.add_component (which cannot pre-flight).
  SpawnMaterialUtils::FMaterialSpec MaterialSpec;
  SpawnMaterialUtils::FResolvedMaterials ResolvedMaterials;
  {
    bool bMaterialPresent = false;
    FString MatErrCode;
    FString MatErrMsg;
    if (!SpawnMaterialUtils::ParseAndResolve(Payload, MaterialSpec, ResolvedMaterials,
                                             bMaterialPresent, MatErrCode, MatErrMsg)) {
      Ctx.SendError(MatErrCode, MatErrMsg);
      return true;
    }
  }
  TArray<FString> MaterialWarnings;
  FString MaterialComponentName;
  int32 MaterialSlotsApplied = 0;

  // Skip LoadAsset for script classes (e.g. /Script/Engine.CameraActor) to
  // avoid LogEditorAssetSubsystem errors
  if ((ClassPath.StartsWith(TEXT("/")) || ClassPath.Contains(TEXT("/"))) &&
      !ClassPath.StartsWith(TEXT("/Script/"))) {
    if (UObject *Loaded = ResolveAsset(ClassPath, /*bLoadObject=*/true).Object) {
      if (UBlueprint *BP = Cast<UBlueprint>(Loaded))
        ResolvedClass = BP->GeneratedClass;
      else if (UClass *C = Cast<UClass>(Loaded))
        ResolvedClass = C;
      else if (UStaticMesh *Mesh = Cast<UStaticMesh>(Loaded))
        ResolvedStaticMesh = Mesh;
      else if (USkeletalMesh *SkelMesh = Cast<USkeletalMesh>(Loaded))
        ResolvedSkeletalMesh = SkelMesh;
    }
  }
  if (!ResolvedClass && !ResolvedStaticMesh && !ResolvedSkeletalMesh)
    ResolvedClass = ResolveClassByName(ClassPath);

  // If explicit mesh path provided for a general spawn request
  if (!ResolvedStaticMesh && !ResolvedSkeletalMesh && !MeshPath.IsEmpty()) {
    if (UObject *MeshObj = ResolveAsset(MeshPath, /*bLoadObject=*/true).Object) {
      ResolvedStaticMesh = Cast<UStaticMesh>(MeshObj);
      if (!ResolvedStaticMesh)
        ResolvedSkeletalMesh = Cast<USkeletalMesh>(MeshObj);
    }
  }

  // Force StaticMeshActor if we have a resolved mesh, regardless of class input
  // (unless it's a specific subclass)
  bool bSpawnStaticMeshActor = (ResolvedStaticMesh != nullptr);
  bool bSpawnSkeletalMeshActor = (ResolvedSkeletalMesh != nullptr);

  if (!bSpawnStaticMeshActor && !bSpawnSkeletalMeshActor && ResolvedClass) {
    bSpawnStaticMeshActor =
        ResolvedClass->IsChildOf(AStaticMeshActor::StaticClass());
    if (!bSpawnStaticMeshActor)
      bSpawnSkeletalMeshActor =
          ResolvedClass->IsChildOf(ASkeletalMeshActor::StaticClass());
  }

  // Explicitly use StaticMeshActor class if we have a mesh but no class
  if (bSpawnStaticMeshActor && !ResolvedClass) {
    ResolvedClass = AStaticMeshActor::StaticClass();
  } else if (bSpawnSkeletalMeshActor && !ResolvedClass) {
    ResolvedClass = ASkeletalMeshActor::StaticClass();
  }

  if (!ResolvedClass && !bSpawnStaticMeshActor && !bSpawnSkeletalMeshActor) {
    Ctx.SendError(TEXT("CLASS_NOT_FOUND"),
        FString::Printf(TEXT("Class not found: %s. Verify plugin is enabled if using a plugin class."), *ClassPath));
    return true;
  }

  AActor *Spawned = nullptr;

  UWorld* TargetWorld = ResolveSpawnWorld();
  if (!TargetWorld) {
    Ctx.SendError(TEXT("WORLD_NOT_FOUND"), TEXT("No valid editor world available for spawn"));
    return true;
  }

  UClass *ClassToSpawn =
      ResolvedClass
          ? ResolvedClass
          : (bSpawnStaticMeshActor ? AStaticMeshActor::StaticClass()
                                   : (bSpawnSkeletalMeshActor
                                          ? ASkeletalMeshActor::StaticClass()
                                          : AActor::StaticClass()));
  Spawned = SpawnActorInWorld(TargetWorld, ClassToSpawn, Location, Rotation);

  if (Spawned) {
    Spawned->SetActorTransform(FTransform(Rotation, Location, Scale), false,
                               nullptr, ETeleportType::TeleportPhysics);

    if (bSpawnStaticMeshActor) {
      if (AStaticMeshActor *StaticMeshActor = Cast<AStaticMeshActor>(Spawned)) {
        if (UStaticMeshComponent *MeshComponent =
                StaticMeshActor->GetStaticMeshComponent()) {
          if (ResolvedStaticMesh) {
            MeshComponent->SetStaticMesh(ResolvedStaticMesh);
          }
          MeshComponent->SetMobility(EComponentMobility::Movable);
          MeshComponent->MarkRenderStateDirty();
        }
      }
    } else if (bSpawnSkeletalMeshActor) {
      if (ASkeletalMeshActor *SkelActor = Cast<ASkeletalMeshActor>(Spawned)) {
        if (USkeletalMeshComponent *SkelComp =
                SkelActor->GetSkeletalMeshComponent()) {
          if (ResolvedSkeletalMesh) {
            SkelComp->SetSkeletalMesh(ResolvedSkeletalMesh);
          }
          SkelComp->SetMobility(EComponentMobility::Movable);
          SkelComp->MarkRenderStateDirty();
        }
      }
    } else if (AVolume *VolumeActor = Cast<AVolume>(Spawned)) {
      // An AVolume carries its shape in a UModel brush, not in its transform, and the
      // raw World->SpawnActor above leaves that model null. The actor then lands with
      // bounds extent (0,0,0) and no collision, every containment query against it
      // answers empty, and nothing downstream notices because
      // UEditorBrushBuilder::EndBrush early-returns success on a null model — so this
      // verb reported a spawned volume that occupies no space. Build the same default
      // 200-uu box the editor gives a volume dragged in from the place-actors panel
      // (UActorFactoryBoxVolume's default-constructed UCubeBuilder); the Scale applied
      // above multiplies it, and volume.set_volume_extent resizes it outright.
      // B-spawned-volumes-have-no-brush-geometry.
      VolumeBrushGeometry::BuildBoxBrushGeometry(
          VolumeActor, FVector(VolumeBrushGeometry::DefaultBoxSize));
    }
  }

  if (!Spawned) {
    Ctx.SendError(TEXT("SPAWN_FAILED"), TEXT("Failed to spawn actor"));
    return true;
  }

  if (!ActorName.IsEmpty()) {
    Spawned->SetActorLabel(ActorName);
  } else {
    // Auto-generate a friendly label from the mesh or class name
    FString BaseName;
    if (ResolvedStaticMesh) {
      BaseName = ResolvedStaticMesh->GetName();
    } else if (ResolvedSkeletalMesh) {
      BaseName = ResolvedSkeletalMesh->GetName();
    } else if (ResolvedClass) {
      BaseName = ResolvedClass->GetName();
      if (BaseName.EndsWith(TEXT("_C"))) {
        BaseName.RemoveFromEnd(TEXT("_C"));
      }
    } else {
      BaseName = TEXT("Actor");
    }
    Spawned->SetActorLabel(BaseName);
  }

  // Bind the pre-resolved materials onto the spawned actor's mesh component. Non-fatal
  // misses (no mesh component, slot out of range) land in MaterialWarnings and are echoed
  // in the response rather than failing a spawn that already succeeded.
  MaterialSlotsApplied = SpawnMaterialUtils::Apply(
      Spawned, ResolvedMaterials, MaterialWarnings, MaterialComponentName);
  SpawnMaterialUtils::NotifySceneMaterialsModified(MaterialSlotsApplied);

  // Build response matching the outputWithActor schema
  TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();

  TSharedPtr<FJsonObject> ActorObj = MakeShared<FJsonObject>();
  // Both identities: 'name' historically carried the LABEL here and keeps that meaning for
  // existing callers, but labels are not unique — UE happily gives two actors the same one,
  // so feeding it back into a lookup can resolve a different actor. 'objectName' is
  // GetName(), unique within the level, and is the collision-safe key.
  ActorObj->SetStringField(TEXT("id"), Spawned->GetPathName());
  ActorObj->SetStringField(TEXT("name"), Spawned->GetActorLabel());
  ActorObj->SetStringField(TEXT("label"), Spawned->GetActorLabel());
  ActorObj->SetStringField(TEXT("objectName"), Spawned->GetName());
  ActorObj->SetStringField(TEXT("path"), Spawned->GetPathName());
  Data->SetObjectField(TEXT("actor"), ActorObj);

  Data->SetStringField(TEXT("actorPath"), Spawned->GetPathName());

  if (ResolvedClass)
    Data->SetStringField(TEXT("classPath"), ResolvedClass->GetPathName());
  else
    Data->SetStringField(TEXT("classPath"), ClassPath);

  if (ResolvedStaticMesh)
    Data->SetStringField(TEXT("meshPath"), ResolvedStaticMesh->GetPathName());
  else if (ResolvedSkeletalMesh)
    Data->SetStringField(TEXT("meshPath"), ResolvedSkeletalMesh->GetPathName());

  SpawnMaterialUtils::AddMaterialReport(Data, MaterialSpec, MaterialSlotsApplied,
                                        MaterialComponentName, MaterialWarnings);

  AddActorVerification(Data, Spawned);

  UE_LOG(LogPinWrightSubsystem, Display,
         TEXT("actor.spawn: Spawned actor '%s'"), *Spawned->GetActorLabel());

  Ctx.SendSuccess(Data);
  return true;

}

// ---- actor.spawn_from_blueprint ----
REGISTER_RPC_HANDLER("actor.spawn_from_blueprint", "actor", "Spawn an instance of a Blueprint class into the editor world by asset path. Resolves the BP's GeneratedClass and uses TeleportPhysics so initial physics state is not interpolated. Errors with CLASS_NOT_FOUND if the path does not resolve to a Blueprint.",
    RPC_PARAMS(
        RPC_PARAM_REQ("blueprintPath", "path", "Asset path to a Blueprint, e.g. '/Game/Foo/BP_Bar' or '/Game/Foo/BP_Bar.BP_Bar'."),
        SpawnParamUtils::SpawnLabelParamOpt(TEXT("Display label for the spawned instance; UE auto-generates one when omitted.")),
        RPC_PARAM_OPT("location", "object", "World-space location as {x,y,z} in unreal units (cm); defaults to origin."),
        RPC_PARAM_OPT("rotation", "object", "World-space rotation as {pitch,yaw,roll} in degrees; defaults to identity."),
        RPC_PARAM_OPT("scale", "object", "World-space 3D scale as {x,y,z} (1.0 = identity); defaults to (1,1,1). Spawn scaled directly instead of a follow-up actor.set_transform.")
    ))
{
  auto* Subsystem = Ctx.GetSubsystem();
  const auto& Payload = Ctx.GetRawPayload();

  FString BlueprintPath = Ctx.GetString(TEXT("blueprintPath"));
  if (BlueprintPath.IsEmpty()) {
    Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("Blueprint path required"));
    return true;
  }

  FString ActorName = SpawnParamUtils::ResolveSpawnLabel(Ctx);
  FVector Location = Ctx.GetVector(TEXT("location"), FVector::ZeroVector);
  FRotator Rotation = Ctx.GetRotator(TEXT("rotation"), FRotator::ZeroRotator);
  FVector Scale = Ctx.GetVector(TEXT("scale"), FVector::OneVector);

  UClass *ResolvedClass = nullptr;

  // Prefer the same blueprint resolution heuristics used by manage_blueprint
  FString NormalizedPath;
  FString LoadError;
  if (!BlueprintPath.IsEmpty()) {
    UBlueprint *BlueprintAsset =
        LoadBlueprintAsset(BlueprintPath, NormalizedPath, LoadError);
    if (BlueprintAsset && BlueprintAsset->GeneratedClass) {
      ResolvedClass = BlueprintAsset->GeneratedClass;
    }
  }

  if (!ResolvedClass && (BlueprintPath.StartsWith(TEXT("/")) ||
                         BlueprintPath.Contains(TEXT("/")))) {
    if (UObject *Loaded = ResolveAsset(BlueprintPath, /*bLoadObject=*/true).Object) {
      if (UBlueprint *BP = Cast<UBlueprint>(Loaded))
        ResolvedClass = BP->GeneratedClass;
      else if (UClass *C = Cast<UClass>(Loaded))
        ResolvedClass = C;
    }
  }
  if (!ResolvedClass)
    ResolvedClass = ResolveClassByName(BlueprintPath);

  if (!ResolvedClass) {
    Ctx.SendError(TEXT("CLASS_NOT_FOUND"), TEXT("Blueprint class not found"));
    return true;
  }

  UE_LOG(LogPinWrightSubsystem, Display,
         TEXT("actor.spawn_from_blueprint: Location=(%f, %f, %f) Rotation=(%f, %f, %f)"),
         Location.X, Location.Y, Location.Z, Rotation.Pitch, Rotation.Yaw,
         Rotation.Roll);

  AActor *Spawned = nullptr;
  UWorld* TargetWorld = ResolveSpawnWorld();
  if (!TargetWorld) {
    Ctx.SendError(TEXT("WORLD_NOT_FOUND"), TEXT("No valid editor world available for spawn"));
    return true;
  }

  Spawned = SpawnActorInWorld(TargetWorld, ResolvedClass, Location, Rotation);
  if (Spawned) {
    Spawned->SetActorTransform(FTransform(Rotation, Location, Scale), false,
                               nullptr, ETeleportType::TeleportPhysics);
  }

  if (!Spawned) {
    Ctx.SendError(TEXT("SPAWN_FAILED"), TEXT("Failed to spawn blueprint"));
    return true;
  }

  if (!ActorName.IsEmpty())
    Spawned->SetActorLabel(ActorName);

  TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();

  TSharedPtr<FJsonObject> ActorObj = MakeShared<FJsonObject>();
  // Both identities: 'name' historically carried the LABEL here and keeps that meaning for
  // existing callers, but labels are not unique — UE happily gives two actors the same one,
  // so feeding it back into a lookup can resolve a different actor. 'objectName' is
  // GetName(), unique within the level, and is the collision-safe key.
  ActorObj->SetStringField(TEXT("id"), Spawned->GetPathName());
  ActorObj->SetStringField(TEXT("name"), Spawned->GetActorLabel());
  ActorObj->SetStringField(TEXT("label"), Spawned->GetActorLabel());
  ActorObj->SetStringField(TEXT("objectName"), Spawned->GetName());
  ActorObj->SetStringField(TEXT("path"), Spawned->GetPathName());
  Resp->SetObjectField(TEXT("actor"), ActorObj);

  Resp->SetStringField(TEXT("actorPath"), Spawned->GetPathName());
  Resp->SetStringField(TEXT("classPath"), ResolvedClass->GetPathName());

  AddActorVerification(Resp, Spawned);

  UE_LOG(LogPinWrightSubsystem, Display,
         TEXT("actor.spawn_from_blueprint: Spawned blueprint '%s'"),
         *Spawned->GetActorLabel());
  Ctx.SendSuccess(Resp);
  return true;
}
