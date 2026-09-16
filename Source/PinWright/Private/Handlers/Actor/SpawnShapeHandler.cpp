// Copyright (c) 2026 Alexander Penkin. MIT License.

// SpawnShapeHandler.cpp - actor.spawn_shape
// Convenience wrapper over actor.spawn for the /Engine/BasicShapes primitives:
// resolves a shape enum (CUBE/SPHERE/CYLINDER/CONE/PLANE) to its engine mesh and
// spawns a StaticMeshActor, returning the same result shape as actor.spawn.

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ParamSpec.h"
#include "Handlers/Actor/SpawnParamUtils.h"
#include "Handlers/Actor/ShapeSpawnUtils.h"
#include "Handlers/Actor/SpawnMaterialUtils.h"
#include "PinWrightGlobals.h"
#include "PinWrightHelpers.h"
#include "PinWrightSubsystem.h"
#include "Utils/AssetUtils.h"
#include "Dom/JsonObject.h"

#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "GameFramework/Actor.h"

// ---- actor.spawn_shape ----
REGISTER_RPC_HANDLER("actor.spawn_shape", "actor", "Spawn a /Engine/BasicShapes primitive (cube, sphere, cylinder, cone, plane) as a StaticMeshActor in one call. A convenience wrapper over actor.spawn that resolves the shape enum to its engine mesh; returns the same actor result shape as actor.spawn.",
    RPC_PARAMS(
        RPC_PARAM_REQ("shape", "string", "Primitive to spawn (case-insensitive): CUBE, SPHERE, CYLINDER, CONE, or PLANE. Maps to the matching /Engine/BasicShapes mesh."),
        SpawnParamUtils::SpawnLabelParamOpt(TEXT("Display label for the spawned actor; defaults to the shape's mesh name (e.g. 'Cube') when omitted.")),
        RPC_PARAM_OPT("location", "object", "World-space location as {x,y,z} in unreal units (cm); defaults to origin."),
        RPC_PARAM_OPT("rotation", "object", "World-space rotation as {pitch,yaw,roll} in degrees; defaults to identity."),
        RPC_PARAM_OPT("scale", "object", "World-space 3D scale as {x,y,z} (1.0 = identity); defaults to (1,1,1)."),
        SpawnMaterialUtils::MaterialPathParam(),
        SpawnMaterialUtils::MaterialPathsParam()
    ))
{
  const FString Shape = Ctx.GetString(TEXT("shape"));
  if (Shape.IsEmpty())
  {
    Ctx.SendError(TEXT("INVALID_PARAMS"),
        FString::Printf(TEXT("shape is required. Valid shapes: %s"), *ShapeSpawnUtils::ValidShapesCsv()));
    return true;
  }

  FString MeshPath;
  if (!ShapeSpawnUtils::ResolveShapeMeshPath(Shape, MeshPath))
  {
    Ctx.SendError(TEXT("INVALID_PARAMS"),
        FString::Printf(TEXT("Unknown shape '%s'. Valid shapes: %s"), *Shape, *ShapeSpawnUtils::ValidShapesCsv()));
    return true;
  }

  UStaticMesh* Mesh = Cast<UStaticMesh>(ResolveAsset(MeshPath, /*bLoadObject=*/true).Object);
  if (!Mesh)
  {
    Ctx.SendError(TEXT("ASSET_LOAD_FAILED"),
        FString::Printf(TEXT("Failed to load shape mesh: %s"), *MeshPath));
    return true;
  }

  // Resolve materialPath / materialPaths before spawning so a bad path never leaves an
  // orphan actor behind (see SpawnMaterialUtils.h).
  SpawnMaterialUtils::FMaterialSpec MaterialSpec;
  SpawnMaterialUtils::FResolvedMaterials ResolvedMaterials;
  {
    bool bMaterialPresent = false;
    FString MatErrCode;
    FString MatErrMsg;
    if (!SpawnMaterialUtils::ParseAndResolve(Ctx.GetRawPayload(), MaterialSpec,
                                             ResolvedMaterials, bMaterialPresent,
                                             MatErrCode, MatErrMsg))
    {
      Ctx.SendError(MatErrCode, MatErrMsg);
      return true;
    }
  }

  UWorld* World = ShapeSpawnUtils::ResolveEditorWorld();
  if (!World)
  {
    Ctx.SendError(TEXT("WORLD_NOT_FOUND"), TEXT("No valid editor world available for spawn"));
    return true;
  }

  const FVector Location = Ctx.GetVector(TEXT("location"), FVector::ZeroVector);
  const FRotator Rotation = Ctx.GetRotator(TEXT("rotation"), FRotator::ZeroRotator);
  const FVector Scale = Ctx.GetVector(TEXT("scale"), FVector::OneVector);

  const FString ActorName = SpawnParamUtils::ResolveSpawnLabel(Ctx);
  const FString Label = ActorName.IsEmpty() ? Mesh->GetName() : ActorName;

  AActor* Spawned = ShapeSpawnUtils::SpawnStaticMeshActor(
      World, Mesh, FTransform(Rotation, Location, Scale), Label);
  if (!Spawned)
  {
    Ctx.SendError(TEXT("SPAWN_FAILED"), TEXT("Failed to spawn shape actor"));
    return true;
  }

  TArray<FString> MaterialWarnings;
  FString MaterialComponentName;
  const int32 MaterialSlotsApplied = SpawnMaterialUtils::Apply(
      Spawned, ResolvedMaterials, MaterialWarnings, MaterialComponentName);
  SpawnMaterialUtils::NotifySceneMaterialsModified(MaterialSlotsApplied);

  // Response mirrors actor.spawn's outputWithActor schema.
  TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();

  TSharedPtr<FJsonObject> ActorObj = MakeShared<FJsonObject>();
  // Both identities: 'name' historically carried the LABEL here and keeps that meaning for
  // existing callers, but labels are not unique — spawning the same shape twice yields two
  // actors labelled from the same mesh name, so feeding a label back into a lookup can
  // resolve the wrong one. 'objectName' is GetName(), unique within the level.
  ActorObj->SetStringField(TEXT("id"), Spawned->GetPathName());
  ActorObj->SetStringField(TEXT("name"), Spawned->GetActorLabel());
  ActorObj->SetStringField(TEXT("label"), Spawned->GetActorLabel());
  ActorObj->SetStringField(TEXT("objectName"), Spawned->GetName());
  ActorObj->SetStringField(TEXT("path"), Spawned->GetPathName());
  Data->SetObjectField(TEXT("actor"), ActorObj);

  Data->SetStringField(TEXT("actorPath"), Spawned->GetPathName());
  Data->SetStringField(TEXT("classPath"), AStaticMeshActor::StaticClass()->GetPathName());
  Data->SetStringField(TEXT("meshPath"), Mesh->GetPathName());
  Data->SetStringField(TEXT("shape"), Shape.TrimStartAndEnd().ToUpper());

  SpawnMaterialUtils::AddMaterialReport(Data, MaterialSpec, MaterialSlotsApplied,
                                        MaterialComponentName, MaterialWarnings);

  AddActorVerification(Data, Spawned);

  UE_LOG(LogPinWrightSubsystem, Display,
         TEXT("actor.spawn_shape: Spawned %s as '%s'"), *Shape, *Spawned->GetActorLabel());
  Ctx.SendSuccess(Data);
  return true;
}
