// Copyright (c) 2026 Alexander Penkin. MIT License.

// SpawnBatchHandler.cpp - actor.spawn_batch
// One call, many actors: spawns or duplicates one actor per entry in transforms[]
// from a single source (shape / meshPath / classPath / sourceActor), with an optional
// World Outliner folder assigned to every result. Capped per call.

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ParamSpec.h"
#include "Handlers/Actor/ShapeSpawnUtils.h"
#include "Handlers/Actor/SpawnMaterialUtils.h"
#include "PinWrightGlobals.h"
#include "PinWrightHelpers.h"
#include "PinWrightSubsystem.h"
#include "Utils/ActorUtils.h"
#include "Utils/ClassUtils.h"
#include "Utils/JsonUtils.h"
#include "Utils/AssetUtils.h"
#include "Dom/JsonObject.h"

#include "Editor.h"
#if __has_include("Subsystems/EditorActorSubsystem.h")
#include "Subsystems/EditorActorSubsystem.h"
#elif __has_include("EditorActorSubsystem.h")
#include "EditorActorSubsystem.h"
#endif
#include "Engine/Blueprint.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "GameFramework/Actor.h"

// ---- actor.spawn_batch ----
REGISTER_RPC_HANDLER("actor.spawn_batch", "actor", "Spawn or duplicate many actors in one call, one per entry in transforms[]. The source is exactly one of: shape (a /Engine/BasicShapes primitive), meshPath (static or skeletal mesh), classPath (UClass or Blueprint), or sourceActor (an existing actor to duplicate). Optionally assigns every spawned actor to a World Outliner folder. Capped at 512 per call. Entries that produce no actor are reported in skipped[] ({index, reason}, capped) with the true total in skippedCount - they are never dropped silently.",
    RPC_PARAMS(
        RPC_PARAM_OPT("shape", "string", "Source: /Engine/BasicShapes primitive (CUBE/SPHERE/CYLINDER/CONE/PLANE, case-insensitive). Mutually exclusive with meshPath/classPath/sourceActor."),
        RPC_PARAM_OPT("meshPath", "path", "Source: static or skeletal mesh asset path (e.g. /Game/Meshes/SM_Wall). Mutually exclusive with the other sources."),
        RPC_PARAM_OPT("classPath", "classref", "Source: UClass name, /Script path, or BP asset path to instance. Mutually exclusive with the other sources."),
        RPC_PARAM_OPT("sourceActor", "string", "Source: display label/name of an existing actor to duplicate for every transform. Mutually exclusive with the other sources."),
        RPC_PARAM_REQ("transforms", "array", "Placements, one spawned actor each: [{location:{x,y,z}, rotation?:{pitch,yaw,roll}, scale?:{x,y,z}, name?:string, materialPath?:string, materialPaths?:string[]}]. Required, non-empty, capped at 512. Missing rotation/scale default to identity/(1,1,1); a missing location defaults to the world origin; missing name auto-labels '<base>_<i>'; missing materialPath/materialPaths inherits the batch-level default. An entry that is not a JSON object, or whose spawn fails, is reported in the response's skipped[] array."),
        RPC_PARAM_OPT("folder", "string", "World Outliner folder path assigned to every spawned actor (e.g. 'Prototype/Walls')."),
        SpawnMaterialUtils::MaterialPathParam(),
        SpawnMaterialUtils::MaterialPathsParam()
    ))
{
  const TArray<TSharedPtr<FJsonValue>>* Transforms = Ctx.GetArray(TEXT("transforms"));
  if (!Transforms || Transforms->Num() == 0)
  {
    Ctx.SendError(TEXT("INVALID_PARAMS"), TEXT("transforms is required and must be a non-empty array"));
    return true;
  }

  constexpr int32 MaxBatch = 512;
  if (Transforms->Num() > MaxBatch)
  {
    Ctx.SendError(TEXT("INVALID_PARAMS"),
        FString::Printf(TEXT("transforms has %d entries; the per-call cap is %d. Split into multiple calls."),
            Transforms->Num(), MaxBatch));
    return true;
  }

  // Exactly one source must be specified.
  const FString Shape = Ctx.GetString(TEXT("shape"));
  const FString MeshPath = Ctx.GetString(TEXT("meshPath"));
  const FString ClassPath = Ctx.GetString(TEXT("classPath"));
  const FString SourceActorName = Ctx.GetString(TEXT("sourceActor"));

  int32 SourceCount = 0;
  SourceCount += Shape.IsEmpty() ? 0 : 1;
  SourceCount += MeshPath.IsEmpty() ? 0 : 1;
  SourceCount += ClassPath.IsEmpty() ? 0 : 1;
  SourceCount += SourceActorName.IsEmpty() ? 0 : 1;
  if (SourceCount != 1)
  {
    Ctx.SendError(TEXT("INVALID_PARAMS"),
        TEXT("Provide exactly one source: shape, meshPath, classPath, or sourceActor."));
    return true;
  }

  UWorld* World = ShapeSpawnUtils::ResolveEditorWorld();
  if (!World)
  {
    Ctx.SendError(TEXT("WORLD_NOT_FOUND"), TEXT("No valid editor world available for spawn"));
    return true;
  }

  // Resolve the source once, up front.
  UStaticMesh* StaticMesh = nullptr;
  USkeletalMesh* SkeletalMesh = nullptr;
  UClass* SpawnClass = nullptr;
  AActor* SourceActor = nullptr;
  FString BaseLabel;

  if (!Shape.IsEmpty())
  {
    FString ShapeMeshPath;
    if (!ShapeSpawnUtils::ResolveShapeMeshPath(Shape, ShapeMeshPath))
    {
      Ctx.SendError(TEXT("INVALID_PARAMS"),
          FString::Printf(TEXT("Unknown shape '%s'. Valid shapes: %s"), *Shape, *ShapeSpawnUtils::ValidShapesCsv()));
      return true;
    }
    StaticMesh = Cast<UStaticMesh>(ResolveAsset(ShapeMeshPath, /*bLoadObject=*/true).Object);
    if (!StaticMesh)
    {
      Ctx.SendError(TEXT("ASSET_LOAD_FAILED"), FString::Printf(TEXT("Failed to load shape mesh: %s"), *ShapeMeshPath));
      return true;
    }
    BaseLabel = StaticMesh->GetName();
  }
  else if (!MeshPath.IsEmpty())
  {
    if (UObject* MeshObj = ResolveAsset(MeshPath, /*bLoadObject=*/true).Object)
    {
      StaticMesh = Cast<UStaticMesh>(MeshObj);
      if (!StaticMesh)
      {
        SkeletalMesh = Cast<USkeletalMesh>(MeshObj);
      }
    }
    if (!StaticMesh && !SkeletalMesh)
    {
      Ctx.SendError(TEXT("ASSET_LOAD_FAILED"),
          FString::Printf(TEXT("meshPath did not resolve to a static or skeletal mesh: %s"), *MeshPath));
      return true;
    }
    BaseLabel = StaticMesh ? StaticMesh->GetName() : SkeletalMesh->GetName();
  }
  else if (!ClassPath.IsEmpty())
  {
    // Same resolution heuristics as actor.spawn: load BP/UClass assets by path, else
    // resolve script/plugin classes by name.
    if ((ClassPath.StartsWith(TEXT("/")) || ClassPath.Contains(TEXT("/"))) &&
        !ClassPath.StartsWith(TEXT("/Script/")))
    {
      if (UObject* Loaded = ResolveAsset(ClassPath, /*bLoadObject=*/true).Object)
      {
        if (UBlueprint* BP = Cast<UBlueprint>(Loaded))
        {
          SpawnClass = BP->GeneratedClass;
        }
        else if (UClass* C = Cast<UClass>(Loaded))
        {
          SpawnClass = C;
        }
      }
    }
    if (!SpawnClass)
    {
      SpawnClass = ResolveClassByName(ClassPath);
    }
    if (!SpawnClass)
    {
      Ctx.SendError(TEXT("CLASS_NOT_FOUND"), FString::Printf(TEXT("Class not found: %s"), *ClassPath));
      return true;
    }
    BaseLabel = SpawnClass->GetName();
    if (BaseLabel.EndsWith(TEXT("_C")))
    {
      BaseLabel.RemoveFromEnd(TEXT("_C"));
    }
  }
  else // sourceActor
  {
    SourceActor = McpActorUtils::FindActorByName(nullptr, SourceActorName);
    if (!SourceActor)
    {
      Ctx.SendError(TEXT("ACTOR_NOT_FOUND"), FString::Printf(TEXT("Source actor not found: %s"), *SourceActorName));
      return true;
    }
    BaseLabel = SourceActor->GetActorLabel();
  }

  const FString Folder = Ctx.GetString(TEXT("folder"));
  UEditorActorSubsystem* ActorSS =
      GEditor ? GEditor->GetEditorSubsystem<UEditorActorSubsystem>() : nullptr;

  // Materials, resolved in ONE pre-pass before any actor is spawned: the batch-level
  // default, then each transforms[] entry's optional override. Any bad path aborts the
  // whole call with a single typed error naming the path and the entry - this verb already
  // reports malformed entries as skips, and a per-entry material failure discovered mid-loop
  // would be a second failure mode on a half-spawned batch. Consistent with how the
  // per-entry `name` slot overrides the auto-label: an entry that supplies either material
  // key replaces the batch default wholesale (it is not merged slot-by-slot).
  SpawnMaterialUtils::FMaterialSpec BatchMaterialSpec;
  SpawnMaterialUtils::FResolvedMaterials BatchMaterials;
  {
    bool bBatchMaterialPresent = false;
    FString MatErrCode;
    FString MatErrMsg;
    if (!SpawnMaterialUtils::ParseAndResolve(Ctx.GetRawPayload(), BatchMaterialSpec,
                                             BatchMaterials, bBatchMaterialPresent,
                                             MatErrCode, MatErrMsg))
    {
      Ctx.SendError(MatErrCode, MatErrMsg);
      return true;
    }
  }

  TArray<SpawnMaterialUtils::FResolvedMaterials> EntryMaterials;
  EntryMaterials.Reserve(Transforms->Num());
  {
    int32 PreIndex = 0;
    for (const TSharedPtr<FJsonValue>& Entry : *Transforms)
    {
      const int32 i = PreIndex++;
      EntryMaterials.Add(BatchMaterials);

      const TSharedPtr<FJsonObject>* EntryObj = nullptr;
      if (!Entry.IsValid() || !Entry->TryGetObject(EntryObj) || !EntryObj ||
          !(*EntryObj).IsValid())
      {
        continue; // malformed entries are skipped (and reported) by the spawn loop below
      }

      SpawnMaterialUtils::FMaterialSpec EntrySpec;
      SpawnMaterialUtils::FResolvedMaterials EntryResolved;
      bool bEntryMaterialPresent = false;
      FString MatErrCode;
      FString MatErrMsg;
      if (!SpawnMaterialUtils::ParseAndResolve(*EntryObj, EntrySpec, EntryResolved,
                                               bEntryMaterialPresent, MatErrCode, MatErrMsg,
                                               FString::Printf(TEXT("transforms[%d]"), i)))
      {
        Ctx.SendError(MatErrCode, MatErrMsg);
        return true;
      }
      if (bEntryMaterialPresent)
      {
        EntryMaterials[i] = EntryResolved;
      }
    }
  }

  TArray<FString> MaterialWarnings;
  int32 TotalMaterialSlotsApplied = 0;
  int32 ActorsWithMaterial = 0;

  // Partial-success reporting: an entry that produced no actor is reported with its
  // index and a reason instead of vanishing. Callers previously had to infer the loss
  // from count < requested, with no way to learn WHICH entries went or why. Shape
  // follows the established partial-success idiom - actor.set_folder's updated/missing
  // and material.authoring.set_material_instance_parameters' applied/failed entries
  // carrying a `reason`. The detail array is capped and paired with the same
  // count/total/truncated vocabulary the bounded list handlers use (actor.list,
  // asset.search), so a 512-entry batch of bad input cannot blow the 10,000-char
  // response budget; skippedCount always carries the true total.
  constexpr int32 MaxSkippedDetail = 32;
  TArray<TSharedPtr<FJsonValue>> SkippedArray;
  int32 SkippedCount = 0;
  auto NoteSkipped = [&](int32 EntryIndex, const TCHAR* Reason)
  {
    ++SkippedCount;
    if (SkippedArray.Num() < MaxSkippedDetail)
    {
      TSharedPtr<FJsonObject> SkipObj = MakeShared<FJsonObject>();
      SkipObj->SetNumberField(TEXT("index"), EntryIndex);
      SkipObj->SetStringField(TEXT("reason"), Reason);
      SkippedArray.Add(MakeShared<FJsonValueObject>(SkipObj));
    }
  };

  TArray<TSharedPtr<FJsonValue>> SpawnedArray;
  int32 Index = 0;
  for (const TSharedPtr<FJsonValue>& Entry : *Transforms)
  {
    const int32 i = Index++;
    const TSharedPtr<FJsonObject>* EntryObj = nullptr;
    if (!Entry.IsValid() || !Entry->TryGetObject(EntryObj) || !EntryObj || !(*EntryObj).IsValid())
    {
      NoteSkipped(i, TEXT("transforms[] entry is not a JSON object"));
      continue; // skip malformed entries
    }

    const FVector Location = ExtractVectorField(*EntryObj, TEXT("location"), FVector::ZeroVector);
    const FRotator Rotation = ExtractRotatorField(*EntryObj, TEXT("rotation"), FRotator::ZeroRotator);
    const FVector Scale = ExtractVectorField(*EntryObj, TEXT("scale"), FVector::OneVector);
    FString EntryName;
    (*EntryObj)->TryGetStringField(TEXT("name"), EntryName);
    const FString Label = EntryName.IsEmpty() ? FString::Printf(TEXT("%s_%d"), *BaseLabel, i) : EntryName;

    const FTransform Xform(Rotation, Location, Scale);
    AActor* NewActor = nullptr;

    if (StaticMesh)
    {
      NewActor = ShapeSpawnUtils::SpawnStaticMeshActor(World, StaticMesh, Xform, Label);
    }
    else if (SkeletalMesh)
    {
      NewActor = ShapeSpawnUtils::SpawnSkeletalMeshActor(World, SkeletalMesh, Xform, Label);
    }
    else if (SpawnClass)
    {
      NewActor = ShapeSpawnUtils::SpawnActorOfClass(World, SpawnClass, Xform, Label);
    }
    else if (SourceActor && ActorSS)
    {
      NewActor = ActorSS->DuplicateActor(SourceActor, World);
      if (NewActor)
      {
        NewActor->SetActorTransform(Xform, false, nullptr, ETeleportType::TeleportPhysics);
        NewActor->SetActorLabel(Label);
      }
    }

    if (!NewActor)
    {
      // Distinguish the three no-actor outcomes. The subsystem-missing case in
      // particular used to silently drop EVERY entry of a sourceActor batch.
      if (SourceActor && !ActorSS)
      {
        NoteSkipped(i, TEXT("duplicate failed: EditorActorSubsystem is unavailable"));
      }
      else if (SourceActor)
      {
        NoteSkipped(i, TEXT("DuplicateActor returned null for the source actor"));
      }
      else
      {
        NoteSkipped(i, TEXT("actor creation returned null for this transform"));
      }
      continue;
    }

    if (!Folder.IsEmpty())
    {
      NewActor->SetFolderPath(FName(*Folder));
    }

    FString EntryComponentName;
    const int32 EntrySlotsApplied = SpawnMaterialUtils::Apply(
        NewActor, EntryMaterials[i], MaterialWarnings, EntryComponentName);
    TotalMaterialSlotsApplied += EntrySlotsApplied;
    if (EntrySlotsApplied > 0)
    {
      ++ActorsWithMaterial;
    }

    TSharedPtr<FJsonObject> SpawnedObj = MakeShared<FJsonObject>();
    // Both identities: 'name' historically carried the LABEL here and keeps that meaning
    // for existing callers, but labels are not unique — a batch that spawns several actors
    // from one base name produces colliding labels, so feeding one back into a lookup can
    // resolve a different actor. 'objectName' is GetName(), unique within the level, and is
    // the collision-safe key to address this row by.
    SpawnedObj->SetStringField(TEXT("name"), NewActor->GetActorLabel());
    SpawnedObj->SetStringField(TEXT("label"), NewActor->GetActorLabel());
    SpawnedObj->SetStringField(TEXT("objectName"), NewActor->GetName());
    SpawnedObj->SetStringField(TEXT("path"), NewActor->GetPathName());
    if (EntryMaterials[i].HasAny())
    {
      SpawnedObj->SetNumberField(TEXT("materialSlotsApplied"), EntrySlotsApplied);
    }
    SpawnedArray.Add(MakeShared<FJsonValueObject>(SpawnedObj));
  }

  TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
  Data->SetArrayField(TEXT("spawned"), SpawnedArray);
  Data->SetNumberField(TEXT("count"), SpawnedArray.Num());
  Data->SetNumberField(TEXT("requested"), Transforms->Num());
  // skippedCount is always present (0 on a clean batch) so a caller can branch on it
  // without probing for an optional key; the detail array and its truncation flag
  // appear only when something was actually dropped.
  Data->SetNumberField(TEXT("skippedCount"), SkippedCount);
  if (SkippedCount > 0)
  {
    Data->SetArrayField(TEXT("skipped"), SkippedArray);
    Data->SetBoolField(TEXT("skippedTruncated"), SkippedArray.Num() < SkippedCount);
  }
  if (!Folder.IsEmpty())
  {
    Data->SetStringField(TEXT("folder"), Folder);
  }

  SpawnMaterialUtils::NotifySceneMaterialsModified(TotalMaterialSlotsApplied);

  if (BatchMaterialSpec.SlotPaths.Num() > 0 || TotalMaterialSlotsApplied > 0 ||
      MaterialWarnings.Num() > 0)
  {
    Data->SetBoolField(TEXT("material_applied"), TotalMaterialSlotsApplied > 0);
    Data->SetNumberField(TEXT("materialSlotsApplied"), TotalMaterialSlotsApplied);
    Data->SetNumberField(TEXT("actorsWithMaterial"), ActorsWithMaterial);
    if (BatchMaterialSpec.SlotPaths.Num() == 1)
    {
      Data->SetStringField(TEXT("materialPath"), BatchMaterialSpec.SlotPaths[0]);
    }
    else if (BatchMaterialSpec.SlotPaths.Num() > 1)
    {
      Data->SetArrayField(TEXT("materialPaths"), EmitStringArray(BatchMaterialSpec.SlotPaths));
    }
    // Parity with actor.spawn / actor.spawn_shape, which get this flag from
    // SpawnMaterialUtils::AddMaterialReport. This handler builds its material report inline,
    // so it derives the flag from the same warning prefix that reporter matches on - one
    // source of truth, no extra out-param threaded through Apply. Written only when true,
    // so a batch that touched no construction-script component is byte-identical to before.
    if (SpawnMaterialUtils::HasConstructionScriptWarning(MaterialWarnings))
    {
      Data->SetBoolField(TEXT("materialOnConstructionScriptComponent"), true);
    }
  }
  if (MaterialWarnings.Num() > 0)
  {
    Data->SetArrayField(TEXT("warnings"), EmitStringArray(MaterialWarnings));
  }

  UE_LOG(LogPinWrightSubsystem, Display,
         TEXT("actor.spawn_batch: Spawned %d/%d actor(s) from base '%s' (%d skipped)"),
         SpawnedArray.Num(), Transforms->Num(), *BaseLabel, SkippedCount);
  Ctx.SendSuccess(Data);
  return true;
}
