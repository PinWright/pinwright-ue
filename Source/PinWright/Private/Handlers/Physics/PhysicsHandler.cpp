// Copyright (c) 2026 Alexander Penkin. MIT License.

// PhysicsHandler.cpp - Migrated from PinWright_AnimationHandlers.cpp
// Phase 15: Physics operations (vehicle config, physics simulation setup, ragdoll)
//
// These were originally sub-actions of HandleAnimationPhysicsAction and
// the standalone HandleSetupRagdoll / HandleActivateRagdoll handlers.
// physics.setup_ragdoll / physics.activate_ragdoll live in PinWrightRuntime's PhysicsHandler.cpp.

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ErrorCodes.h"
#include "Handlers/PackagePathCompose.h"
#include "PinWrightHelpers.h"
#include "PinWrightGlobals.h"
#include "PinWrightSubsystem.h"

#include "Animation/Skeleton.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Editor.h"
#include "Editor/EditorEngine.h"
#include "EngineUtils.h"
#include "RenderingThread.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Utils/AssetUtils.h"
#include "Utils/PieState.h"
#include "EditorAssetLibrary.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "UObject/Package.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"

#if __has_include("Subsystems/EditorActorSubsystem.h")
#include "Subsystems/EditorActorSubsystem.h"
#elif __has_include("EditorActorSubsystem.h")
#include "EditorActorSubsystem.h"
#endif

// ============================================================
// physics.setup_physics_simulation - Create physics asset for a skeletal mesh
// ============================================================
REGISTER_RPC_HANDLER("physics.setup_physics_simulation", "physics",
    "Create a physics asset for a skeletal mesh and optionally assign it",
    RPC_PARAMS(
        RPC_PARAM_OPT("meshPath", "path", "Path to skeletal mesh asset"),
        RPC_PARAM_OPT("skeletonPath", "path", "Path to skeleton (falls back to preview mesh)"),
        RPC_PARAM_OPT("actorName", "string", "Actor name to find skeletal mesh from"),
        RPC_PARAM_OPT("physicsAssetName", "string", "Name for the physics asset"),
        RPC_PARAM_OPT("savePath", "path", "Save directory (default /Game/Physics)"),
        RPC_PARAM_OPT("assignToMesh", "boolean", "Assign created physics asset to the mesh"),
        RPC_PARAM_DEF("save", "boolean", "Write the new PhysicsAsset to disk. If assignToMesh is true, also write the changed SkeletalMesh. false marks packages dirty only; the response reports each measured save state.", "true")
    ))
{
  FString MeshPath = Ctx.GetString("meshPath");
  FString SkeletonPath = Ctx.GetString("skeletonPath");
  FString ActorName = Ctx.GetString("actorName");
  const bool bSave = Ctx.GetBool("save", true);

  // savePath and physicsAssetName are resolved and CHECKED HERE, ahead of the mesh / skeleton /
  // actor resolution below, for two reasons.
  // (1) The composed path is what kills the process. physicsAssetName used to be concatenated
  //     onto savePath with FString::Printf(TEXT("%s/%s"), ...) and handed straight to
  //     CreatePackage, which logs at Fatal - a verbosity not compiled out in any configuration,
  //     so it ends the editor and every unsaved package in it - on a name containing "//" or one
  //     that resolves to empty (UObjectGlobals.cpp:1086-1120). Printf doubles the separator
  //     UNCONDITIONALLY, unlike FString::operator/, so BOTH physicsAssetName: "/Game/X" (a rooted
  //     path, composing "/Game/Physics//Game/X") and physicsAssetName: "a//b" reached that Fatal.
  //     Board B-createpackage-unvalidated-paths-plugin-wide; measured shape on
  //     B-foliage-add-type-name-with-slash-kills-the-editor.
  // (2) The ordering is load-bearing for the regression test: it drives a malformed
  //     physicsAssetName together with a meshPath that resolves to nothing, so on a build where
  //     this check is absent or moved below the mesh resolution the call is refused for the WRONG
  //     reason and the test goes red - instead of reaching CreatePackage and taking the test host
  //     down with it. Do not move this below the mesh resolution.
  // The default name is derived from TargetMesh and so cannot be composed until that resolution
  // has run; it is checked at the same helper immediately after it, where the derivation happens.
  FString PhysicsAssetName = Ctx.GetString("physicsAssetName");

  FString SavePath = Ctx.GetString("savePath");
  if (SavePath.IsEmpty()) {
    SavePath = TEXT("/Game/Physics");
  }
  SavePath = SavePath.TrimStartAndEnd();

  if (!FPackageName::IsValidLongPackageName(SavePath)) {
    FString NormalizedPath;
    if (!FPackageName::TryConvertFilenameToLongPackageName(SavePath, NormalizedPath)) {
      Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, TEXT("Invalid savePath for physics asset"));
      return true;
    }
    SavePath = NormalizedPath;
  }

  // PinWrightComposeAssetPackagePath joins with Printf("%s/%s"), which - unlike
  // FString::operator/, whose PathAppend (String.cpp.inl:855-885) pops the separator first -
  // doubles the separator when the LEFT already ends in '/'. A folder arriving that way would
  // compose the very "//" the helper exists to refuse, and would be refused rather than
  // creating an asset the caller can address. The check above rejects a trailing slash, but
  // TryConvertFilenameToLongPackageName's output on the fallback branch is not re-checked, so
  // normalize here rather than depending on its shape. Placed AFTER the validation, so the set
  // of accepted savePath values is unchanged - this only trims what the fallback produced.
  SavePath.RemoveFromEnd(TEXT("/"));

  FString PhysicsAssetObjectPath;
  if (!PhysicsAssetName.IsEmpty()) {
    FString PhysicsPathError;
    if (!PinWrightComposeAssetPackagePath(SavePath, PhysicsAssetName, PhysicsAssetObjectPath,
                                          PhysicsPathError)) {
      Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
                    FString::Printf(TEXT("%s Pass a bare asset name and choose the folder with "
                                         "savePath (default /Game/Physics)."),
                                    *PhysicsPathError));
      return true;
    }
  }

  if (PinWrightPieState::IsPlayInEditorActive())
  {
    Ctx.SendError(
        ErrorCodes::ERR_PIE_ACTIVE,
        TEXT("physics.setup_physics_simulation cannot run while the editor is in play mode; stop PIE and retry."));
    return true;
  }

  const bool bMeshProvided = !MeshPath.IsEmpty();
  const bool bSkeletonProvided = !SkeletonPath.IsEmpty();
  const bool bActorProvided = !ActorName.IsEmpty();

  bool bMeshLoadFailed = false;
  bool bSkeletonLoadFailed = false;
  bool bSkeletonMissingPreview = false;

  USkeletalMesh *TargetMesh = nullptr;
  bool bMeshTypeMismatch = false;
  FString FoundClassName;

  // If actorName provided, try to find the actor and get its skeletal mesh
  if (!bMeshProvided && !bSkeletonProvided && bActorProvided) {
    UE_LOG(LogPinWrightSubsystem, Display,
           TEXT("Attempting to find actor by name: '%s'"), *ActorName);
    AActor *FoundActor = Ctx.GetSubsystem()->FindActorByName(ActorName);
    if (FoundActor) {
      UE_LOG(LogPinWrightSubsystem, Display,
             TEXT("Found actor: '%s' (Label: '%s')"), *FoundActor->GetName(),
             *FoundActor->GetActorLabel());
      if (USkeletalMeshComponent *SkelComp =
              FoundActor->FindComponentByClass<USkeletalMeshComponent>()) {
        TargetMesh = SkelComp->GetSkeletalMeshAsset();
        if (!TargetMesh) {
          Ctx.SendError(ErrorCodes::ERR_ACTOR_SKELETAL_MESH_ASSET_NULL,
                        FString::Printf(TEXT("Actor '%s' has a SkeletalMeshComponent "
                                             "but no SkeletalMesh asset assigned."),
                                        *FoundActor->GetName()));
          return true;
        }
      } else {
        Ctx.SendError(ErrorCodes::ERR_ACTOR_NO_SKELETAL_MESH_COMPONENT,
                      FString::Printf(TEXT("Actor '%s' does not have a SkeletalMeshComponent."),
                                      *FoundActor->GetName()));
        return true;
      }
    } else {
      Ctx.SendError(ErrorCodes::ERR_ACTOR_NOT_FOUND,
                    FString::Printf(TEXT("Actor '%s' not found."), *ActorName));
      return true;
    }
  }

  if (bMeshProvided) {
    if (ResolveAsset(MeshPath).bExists) {
      UObject *Asset = ResolveAsset(MeshPath, /*bLoadObject=*/true).Object;
      TargetMesh = Cast<USkeletalMesh>(Asset);
      if (!TargetMesh && Asset) {
        bMeshTypeMismatch = true;
        FoundClassName = Asset->GetClass()->GetName();
        UE_LOG(LogPinWrightSubsystem, Warning,
               TEXT("setup_physics_simulation: Asset %s is not a "
                    "SkeletalMesh (Class: %s)"),
               *MeshPath, *FoundClassName);
      } else if (!Asset) {
        bMeshLoadFailed = true;
      }
    } else {
      bMeshLoadFailed = true;
    }
  }

  USkeleton *TargetSkeleton = nullptr;
  if (!TargetMesh && bSkeletonProvided) {
    if (ResolveAsset(SkeletonPath).bExists) {
      TargetSkeleton = LoadObject<USkeleton>(nullptr, *SkeletonPath);
      if (TargetSkeleton) {
        TargetMesh = TargetSkeleton->GetPreviewMesh();
        if (!TargetMesh) {
          bSkeletonMissingPreview = true;
        }
      } else {
        bSkeletonLoadFailed = true;
      }
    } else {
      bSkeletonLoadFailed = true;
    }
  }

  if (!TargetSkeleton && TargetMesh) {
    TargetSkeleton = TargetMesh->GetSkeleton();
  }

  if (!TargetMesh) {
    if (bMeshTypeMismatch) {
      Ctx.SendError(ErrorCodes::ERR_TYPE_MISMATCH,
                    FString::Printf(TEXT("asset found but is not a SkeletalMesh: %s (is %s)"),
                                    *MeshPath, *FoundClassName));
    } else if (bMeshLoadFailed) {
      Ctx.SendError(ErrorCodes::ERR_ASSET_NOT_FOUND,
                    FString::Printf(TEXT("asset not found: skeletal mesh %s"), *MeshPath));
    } else if (bSkeletonLoadFailed) {
      Ctx.SendError(ErrorCodes::ERR_ASSET_NOT_FOUND,
                    FString::Printf(TEXT("asset not found: skeleton %s"), *SkeletonPath));
    } else if (bSkeletonMissingPreview) {
      Ctx.SendError(ErrorCodes::ERR_ASSET_NOT_FOUND,
                    FString::Printf(TEXT("asset not found: skeleton %s (no preview mesh for physics simulation)"),
                                    *SkeletonPath));
    } else {
      Ctx.SendError(ErrorCodes::ERR_ASSET_NOT_FOUND,
                    TEXT("asset not found: no valid skeletal mesh provided for physics simulation setup"));
    }
    return true;
  }

  if (!TargetSkeleton && !SkeletonPath.IsEmpty()) {
    TargetSkeleton = LoadObject<USkeleton>(nullptr, *SkeletonPath);
  }

  // The default name is a UObject name and so carries no separator, but it is composed through
  // the same helper as the caller-supplied one: what protects CreatePackage below is that
  // PhysicsAssetObjectPath has no other producer.
  if (PhysicsAssetName.IsEmpty()) {
    PhysicsAssetName = TargetMesh->GetName() + TEXT("_Physics");
    FString DefaultPathError;
    if (!PinWrightComposeAssetPackagePath(SavePath, PhysicsAssetName, PhysicsAssetObjectPath,
                                          DefaultPathError)) {
      Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, DefaultPathError);
      return true;
    }
  }

  if (!DoesAssetDirectoryExist(SavePath)) {
    UEditorAssetLibrary::MakeDirectory(SavePath);
  }

  // CreatePackage below is NOT the first door to the Fatal on this path. LoadObject ->
  // StaticLoadObjectInternal calls ResolveName2(..., Create=true) (UObjectGlobals.cpp:1427),
  // and ResolveName2 itself calls CreatePackage(*PartialName) (:1310) - so the existence check
  // immediately below reaches the SAME Fatal on the same string, several lines earlier. A guard
  // placed "immediately before CreatePackage" would therefore not be a guard at all: the process
  // would already be gone. That is why both composition guards for PhysicsAssetObjectPath sit
  // near the top of this handler, above everything that touches the composed path. FindObject is
  // the safe counterpart (Create=false, :620); LoadObject is not.
  if (ResolveAsset(PhysicsAssetObjectPath).bExists) {
    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetStringField(TEXT("physicsAssetPath"), PhysicsAssetObjectPath);
    Resp->SetBoolField(TEXT("existingAsset"), true);
    Resp->SetStringField(TEXT("savePath"), SavePath);
    Resp->SetStringField(TEXT("meshPath"), TargetMesh->GetPathName());
    if (TargetSkeleton) {
      Resp->SetStringField(TEXT("skeletonPath"), TargetSkeleton->GetPathName());
    }
    UPhysicsAsset* ExistingPhysicsAsset = LoadObject<UPhysicsAsset>(nullptr, *PhysicsAssetObjectPath);
    if (ExistingPhysicsAsset) {
      AddAssetVerification(Resp, ExistingPhysicsAsset);
    }
    Ctx.SendSuccess(Resp);
    return true;
  }

  // Create the physics asset headlessly via the shared helper, which bypasses
  // UPhysicsAssetFactory's interactive "New Physics Asset" body-generation modal
  // (CreatePhysicsAssetFromMesh -> OpenNewBodyDlg -> GEditor->EditorAddModalWindow) that
  // would wedge the game thread forever in this non-unattended MCP editor
  // (B-physics-asset-factory-modal-hang). See McpCreatePhysicsAssetFromSkeletalMeshHeadless.
  UPackage *PhysicsPackage = CreatePackage(*PhysicsAssetObjectPath);
  if (!PhysicsPackage) {
    Ctx.SendError(ErrorCodes::ERR_ASSET_CREATION_FAILED, TEXT("Failed to create physics asset package"));
    return true;
  }

  const FPhysicsAssetCreateResult Created = McpCreatePhysicsAssetFromSkeletalMeshHeadless(
      PhysicsPackage, *PhysicsAssetName, TargetMesh, bSave);
  if (!Created.bSuccess || !Created.Asset) {
    Ctx.SendError(ErrorCodes::ERR_ASSET_CREATION_FAILED, Created.ErrorMessage);
    return true;
  }
  UPhysicsAsset *PhysicsAsset = Created.Asset;

  const bool bAssignToMesh = Ctx.GetBool("assignToMesh", false);
  FString MeshPackageName;
  int64 MeshSizeBytes = 0;
  EAssetSaveState MeshSaveState = EAssetSaveState::NotRequested;
  bool bMeshSavedToDisk = false;
  if (bAssignToMesh) {
    TargetMesh->Modify();
    TargetMesh->SetPhysicsAsset(PhysicsAsset);
    McpSafeAssetSave(TargetMesh);
    MeshPackageName = TargetMesh->GetOutermost()
        ? TargetMesh->GetOutermost()->GetName() : FString();
    if (bSave) {
      bMeshSavedToDisk = SaveAssetToDiskReportingPresence(
          TargetMesh, /*bForce=*/true, &MeshPackageName, &MeshSizeBytes, &MeshSaveState);
    }
  }

  TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
  Resp->SetStringField(TEXT("physicsAssetPath"), PhysicsAsset->GetPathName());
  Resp->SetBoolField(TEXT("assignedToMesh"), bAssignToMesh);
  Resp->SetBoolField(TEXT("existingAsset"), false);
  Resp->SetStringField(TEXT("savePath"), SavePath);
  Resp->SetStringField(TEXT("meshPath"), TargetMesh->GetPathName());
  Resp->SetStringField(TEXT("package"), Created.PackageName);
  if (TargetSkeleton) {
    Resp->SetStringField(TEXT("skeletonPath"), TargetSkeleton->GetPathName());
  }
  Resp->SetBoolField(TEXT("savedToDisk"), Created.bSavedToDisk);
  Resp->SetBoolField(TEXT("pendingFlush"), Created.bPendingFlush);
  AddAssetSaveSizeReport(Resp, Created.SizeBytes, Created.bSavedToDisk);
  AddAssetSaveReport(Resp, bSave, Created.bSavedToDisk, Created.SaveState);
  if (bAssignToMesh) {
    TSharedPtr<FJsonObject> MeshSave = MakeShared<FJsonObject>();
    MeshSave->SetStringField(TEXT("assetPath"), TargetMesh->GetPathName());
    MeshSave->SetStringField(TEXT("package"), MeshPackageName);
    MeshSave->SetBoolField(TEXT("savedToDisk"), bMeshSavedToDisk);
    MeshSave->SetBoolField(TEXT("pendingFlush"), bSave && !bMeshSavedToDisk);
    AddAssetSaveSizeReport(MeshSave, MeshSizeBytes, bMeshSavedToDisk);
    AddAssetSaveReport(MeshSave, bSave, bMeshSavedToDisk, MeshSaveState);
    Resp->SetObjectField(TEXT("skeletalMeshSave"), MeshSave);
  }
  AddAssetVerification(Resp, PhysicsAsset);
  Ctx.SendSuccess(Resp);
  return true;
}
