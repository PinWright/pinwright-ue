// Copyright (c) 2026 Alexander Penkin. MIT License.

// AnimationHandler.cpp - Migrated from PinWright_AnimationHandlers.cpp
// Phase 15: Animation core operations (cleanup, create BP, blend spaces, state machines,
// asset creation, retargeting, montage playback, notifies)
//
// NOTE: The detailed authoring sub-actions (create_animation_sequence, set_sequence_length,
// add_bone_track, set_curve_key, create_montage, add_montage_section, etc.)
// were already migrated to AnimationAuthoringHandler.cpp under the "animation_authoring.*" prefix.
// This file migrates the remaining OLD compound-handler sub-actions that lived in
// HandleAnimationPhysicsAction and the standalone HandleCreateAnimBlueprint / HandlePlayAnimMontage.

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ErrorCodes.h"
#include "Handlers/ParamAliasUtils.h"
#include "Utils/AssetCreatePolicy.h"
#include "Utils/AssetDeletePolicy.h"
#include "Utils/AssetUtils.h"
#include "Utils/PieState.h"
#include "PinWrightHelpers.h"
#include "PinWrightGlobals.h"
#include "PinWrightSubsystem.h"

#include "Animation/AnimBlueprint.h"
#include "Animation/AnimBlueprintGeneratedClass.h"
#include "Animation/AnimMontage.h"
#include "Animation/AnimSequence.h"
#include "Animation/AnimationAsset.h"
#include "Animation/Skeleton.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"

#if __has_include("Animation/AnimationBlueprintLibrary.h")
#include "Animation/AnimationBlueprintLibrary.h"
#elif __has_include("AnimationBlueprintLibrary.h")
#include "AnimationBlueprintLibrary.h"
#endif
#if __has_include("Animation/AnimBlueprintLibrary.h")
#include "Animation/AnimBlueprintLibrary.h"
#endif
#include "Animation/BlendSpace.h"
#include "Animation/BlendSpace1D.h"
#include "Animation/AimOffsetBlendSpace.h"
#include "Animation/AimOffsetBlendSpace1D.h"
#include "AnimationAuthoringHelpers.h"  // GetBlendParametersForWrite
// Axis config is written to UBlendSpace::BlendParameters via FProperty reflection
// (see ApplyBlendSpaceConfiguration), so the deprecated UBlendSpaceBase header —
// removed in UE 5.7 — is no longer needed here.
#if __has_include("AnimData/IAnimationDataController.h")
#include "AnimData/IAnimationDataController.h"
#endif
#if __has_include("Animation/AnimData/IAnimationDataModel.h")
#include "Animation/AnimData/IAnimationDataModel.h"
#endif
#if __has_include("Animation/AnimData/CurveIdentifier.h")
#include "Animation/AnimData/CurveIdentifier.h"
#endif
#include "Editor.h"
#include "Editor/EditorEngine.h"
#include "EngineUtils.h"
#include "RenderingThread.h"

#if __has_include("Factories/BlendSpaceFactoryNew.h") && \
    __has_include("Factories/BlendSpaceFactory1D.h")
#include "Factories/BlendSpaceFactory1D.h"
#include "Factories/BlendSpaceFactoryNew.h"
#define MCP_HAS_BLENDSPACE_FACTORY 1
#else
#define MCP_HAS_BLENDSPACE_FACTORY 0
#endif
#include "ControlRig.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "EditorAssetLibrary.h"
#include "Factories/AnimBlueprintFactory.h"
#include "Factories/AnimMontageFactory.h"
#include "Factories/AnimSequenceFactory.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "ObjectTools.h"

#if __has_include("Subsystems/EditorActorSubsystem.h")
#include "Subsystems/EditorActorSubsystem.h"
#elif __has_include("EditorActorSubsystem.h")
#include "EditorActorSubsystem.h"
#endif
#if __has_include("Subsystems/AssetEditorSubsystem.h")
#include "Subsystems/AssetEditorSubsystem.h"
#define MCP_HAS_ASSET_EDITOR_SUBSYSTEM 1
#elif __has_include("AssetEditorSubsystem.h")
#include "AssetEditorSubsystem.h"
#define MCP_HAS_ASSET_EDITOR_SUBSYSTEM 1
#else
#define MCP_HAS_ASSET_EDITOR_SUBSYSTEM 0
#endif
#include "UObject/Package.h"
#include "UObject/Script.h"
#include "UObject/UnrealType.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "ScopedTransaction.h"
#include "Utils/BlueprintGraphSnapshot.h"
#include "Utils/TransactionUtils.h"

// State-machine authoring path for animation.create_state_machine. The handler
// used to dispatch legacy console verbs (AddAnimStateMachine/AddAnimState/...)
// through GEditor->Exec; none of those verbs has an Exec consumer, so the method
// failed on command #1 for every input (ticket B-create-state-machine-exec-failed).
// It now drives the same direct-manipulation graph construction that backs the
// animation.authoring.add_state_machine / add_state / add_transition cluster.
#include "AnimGraphConstructionUtils.h"
#include "AnimationHandlerTestHooks.h"
#include "EdGraph/EdGraph.h"
// Require the full state-machine authoring header set so every type the success
// path touches (SM node, state node, transition node, SM graph) is complete
// whenever the gate is on; mirrors the authoring handler's include set. The
// entry-node connection is delegated to AnimGraphConstructionUtils::SetStateMachineEntry,
// so this TU no longer includes AnimStateEntryNode.h directly (it stays in the
// __has_include availability probe below since the helper needs the module).
#if __has_include("AnimGraphNode_StateMachine.h") && __has_include("AnimStateNode.h") && \
    __has_include("AnimStateEntryNode.h") && __has_include("AnimStateTransitionNode.h") && \
    __has_include("AnimationStateMachineGraph.h")
#include "AnimGraphNode_StateMachine.h"
#include "AnimStateNode.h"
#include "AnimStateTransitionNode.h"
#include "AnimationStateMachineGraph.h"
#define MCP_ANIM_HANDLER_HAS_STATE_MACHINE_GRAPH 1
#else
#define MCP_ANIM_HANDLER_HAS_STATE_MACHINE_GRAPH 0
#endif

namespace {

#if MCP_HAS_BLENDSPACE_FACTORY
// Creates a new 1D or 2D Blend Space asset bound to a target skeleton.
static UObject *CreateBlendSpaceAsset(const FString &AssetName,
                                      const FString &PackagePath,
                                      USkeleton *TargetSkeleton,
                                      bool bTwoDimensional, FString &OutError) {
  OutError.Reset();

  UFactory *Factory = nullptr;
  UClass *DesiredClass = nullptr;

  if (bTwoDimensional) {
    UBlendSpaceFactoryNew *Factory2D = NewObject<UBlendSpaceFactoryNew>();
    if (!Factory2D) {
      OutError = TEXT("Failed to allocate BlendSpace factory");
      return nullptr;
    }
    Factory2D->TargetSkeleton = TargetSkeleton;
    Factory = Factory2D;
    DesiredClass = UBlendSpace::StaticClass();
  } else {
    UBlendSpaceFactory1D *Factory1D = NewObject<UBlendSpaceFactory1D>();
    if (!Factory1D) {
      OutError = TEXT("Failed to allocate BlendSpace1D factory");
      return nullptr;
    }
    Factory1D->TargetSkeleton = TargetSkeleton;
    Factory = Factory1D;
    DesiredClass = UBlendSpace1D::StaticClass();
  }

  if (!Factory || !DesiredClass) {
    OutError = TEXT("BlendSpace factory unavailable");
    return nullptr;
  }

  FAssetToolsModule &AssetToolsModule =
      FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools"));
  return AssetToolsModule.Get().CreateAsset(AssetName, PackagePath,
                                            DesiredClass, Factory);
}

// Applies axis range and grid configuration to a blend space asset.
// Returns true if the axis values were written to the asset.
//
// UE 5.7 removed UBlendSpaceBase, and UBlendSpace exposes BlendParameters only as
// a protected array with a const-only GetBlendParameter accessor — so the values
// are written through the shared AnimationAuthoringHelpers::GetBlendParametersForWrite
// reflection helper (single-sourced with create_blend_space_1d/_2d). It works on
// UE 5.3-5.7 because UBlendSpace::BlendParameters has existed throughout
// (UBlendSpace1D derives from UBlendSpace, so a single reflection write covers
// both 1D and 2D).
static bool ApplyBlendSpaceConfiguration(UObject *BlendSpaceAsset,
                                         const TSharedPtr<FJsonObject> &Payload,
                                         bool bTwoDimensional) {
  if (!BlendSpaceAsset || !Payload.IsValid()) {
    return false;
  }

  double MinX = 0.0, MaxX = 1.0, GridX = 3.0;
  Payload->TryGetNumberField(TEXT("minX"), MinX);
  Payload->TryGetNumberField(TEXT("maxX"), MaxX);
  Payload->TryGetNumberField(TEXT("gridX"), GridX);

  UBlendSpace *BlendSpace = Cast<UBlendSpace>(BlendSpaceAsset);
  if (!BlendSpace) {
    UE_LOG(
        LogPinWrightSubsystem, Warning,
        TEXT("ApplyBlendSpaceConfiguration: Asset %s is not a UBlendSpace type"),
        *BlendSpaceAsset->GetName());
    return false;
  }

  BlendSpace->Modify();

  FBlendParameter *BlendParams =
      AnimationAuthoringHelpers::GetBlendParametersForWrite(BlendSpace);
  if (!BlendParams) {
    return false;
  }

  BlendParams[0].Min = static_cast<float>(MinX);
  BlendParams[0].Max = static_cast<float>(MaxX);
  BlendParams[0].GridNum = FMath::Max(1, static_cast<int32>(GridX));

  if (bTwoDimensional) {
    double MinY = 0.0, MaxY = 1.0, GridY = 3.0;
    Payload->TryGetNumberField(TEXT("minY"), MinY);
    Payload->TryGetNumberField(TEXT("maxY"), MaxY);
    Payload->TryGetNumberField(TEXT("gridY"), GridY);

    BlendParams[1].Min = static_cast<float>(MinY);
    BlendParams[1].Max = static_cast<float>(MaxY);
    BlendParams[1].GridNum = FMath::Max(1, static_cast<int32>(GridY));
  }

  BlendSpace->PostEditChange();
  BlendSpace->MarkPackageDirty();
  return true;
}
#endif // MCP_HAS_BLENDSPACE_FACTORY

} // namespace

// ============================================================
// animation.cleanup - Remove animation artifacts
// ============================================================
REGISTER_RPC_HANDLER("animation.cleanup", "animation",
    "Bulk-delete animation assets by path. Closes any open editors for each asset and forces GC before deletion to release references. Reports cleaned, missing (already absent), failed (delete refused) and refused (still referenced) buckets. A still-referenced artifact is refused and left completely untouched unless force:true is passed — cleanup never breaks something that is still in use.",
    RPC_PARAMS(
        RPC_PARAM_REQ("artifacts", "array", "Array of asset path strings (e.g. ['/Game/Anims/AS_Foo','/Game/Anims/ABP_Bar']); empty/non-string entries are skipped."),
        RPC_PARAM_OPT("force", "boolean", "Delete artifacts that something still references; defaults to false, which lists them under refused and changes nothing. force:true runs the engine's Force Delete: it replaces EVERY in-memory pointer to the artifact with null editor-wide and marks each of those packages dirty BEFORE the engine decides whether the .uasset may go — irreversibly (no transaction, no undo), and the file can still survive, leaving the referencers broken. Packages damaged that way are listed in referencesNulled; do not save them, reload them with asset.reload.")
    ))
{
  const TArray<TSharedPtr<FJsonValue>> *ArtifactsArray = Ctx.GetArray("artifacts");
  if (!ArtifactsArray) {
    Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, TEXT("artifacts array required for cleanup"));
    return true;
  }

  if (PinWrightPieState::IsPlayInEditorActive()) {
    Ctx.SendError(
        ErrorCodes::ERR_PIE_ACTIVE,
        TEXT("animation.cleanup cannot run while the editor is in play mode; stop PIE and retry."));
    return true;
  }

  // Opt-in destruction. UEditorAssetLibrary::DeleteAsset funnels into
  // ObjectTools::ForceDeleteObjects, which nulls every in-memory pointer to the artifact
  // and dirties those packages before it knows whether the .uasset may go - and the
  // CloseAllEditorsForAsset + ForceGarbageCollection above deliberately strengthen that.
  // A cleanup verb is the last place that should silently break a live reference; see
  // Utils/AssetDeletePolicy.h.
  const bool bForce = Ctx.GetBool(TEXT("force"), false);

  TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
  TArray<FString> Cleaned;
  TArray<FString> Missing;
  TArray<FString> Failed;
  TArray<FString> Refused;
  TArray<FString> DirtiedPackages;

  for (const TSharedPtr<FJsonValue> &Val : *ArtifactsArray) {
    if (!Val.IsValid() || Val->Type != EJson::String) {
      continue;
    }

    const FString ArtifactPath = Val->AsString().TrimStartAndEnd();
    if (ArtifactPath.IsEmpty()) {
      continue;
    }

    if (ResolveAsset(ArtifactPath).bExists) {
      // Close editors to ensure asset can be deleted
#if MCP_HAS_ASSET_EDITOR_SUBSYSTEM
      if (GEditor) {
        UObject *Asset = LoadObject<UObject>(nullptr, *ArtifactPath);
        if (Asset) {
          if (UAssetEditorSubsystem *AssetEditorSubsystem =
                  GEditor->GetEditorSubsystem<UAssetEditorSubsystem>()) {
            AssetEditorSubsystem->CloseAllEditorsForAsset(Asset);
          }
        }
      }
#endif

      // Flush before deleting to release references
      if (GEditor) {
        FlushRenderingCommands();
        GEditor->ForceGarbageCollection(true);
        FlushRenderingCommands();
      }

      const AssetDeletePolicy::FResult DeleteResult =
          AssetDeletePolicy::DeleteAsset(ArtifactPath, bForce);
      if (DeleteResult.bDeleted) {
        Cleaned.Add(ArtifactPath);
        for (const FString &Package : DeleteResult.DirtiedPackages) {
          DirtiedPackages.AddUnique(Package);
        }
      } else if (DeleteResult.bRefused) {
        Refused.Add(ArtifactPath);
      } else {
        Failed.Add(ArtifactPath);
      }
    } else {
      Missing.Add(ArtifactPath);
    }
  }

  TArray<TSharedPtr<FJsonValue>> CleanedArray;
  for (const FString &Path : Cleaned) {
    CleanedArray.Add(MakeShared<FJsonValueString>(Path));
  }
  if (CleanedArray.Num() > 0) {
    Resp->SetArrayField(TEXT("cleaned"), CleanedArray);
  }
  Resp->SetNumberField(TEXT("cleanedCount"), Cleaned.Num());

  if (Missing.Num() > 0) {
    TArray<TSharedPtr<FJsonValue>> MissingArray;
    for (const FString &Path : Missing) {
      MissingArray.Add(MakeShared<FJsonValueString>(Path));
    }
    Resp->SetArrayField(TEXT("missing"), MissingArray);
  }

  if (Failed.Num() > 0) {
    TArray<TSharedPtr<FJsonValue>> FailedArray;
    for (const FString &Path : Failed) {
      FailedArray.Add(MakeShared<FJsonValueString>(Path));
    }
    Resp->SetArrayField(TEXT("failed"), FailedArray);
  }

  // A refusal is a different outcome from a failure: nothing about the artifact changed,
  // so retrying unchanged can only refuse again. Kept out of `failed` for that reason.
  if (Refused.Num() > 0) {
    TArray<TSharedPtr<FJsonValue>> RefusedArray;
    for (const FString &Path : Refused) {
      RefusedArray.Add(MakeShared<FJsonValueString>(Path));
    }
    Resp->SetArrayField(TEXT("refused"), RefusedArray);
    Resp->SetStringField(
        TEXT("refusedHint"),
        TEXT("Those artifacts are still referenced and were left untouched - no reference "
             "was nulled, no package was dirtied, no file was removed. Repoint or delete "
             "the referencers first, or pass force:true after reading its description."));
  }

  if (DirtiedPackages.Num() > 0) {
    TArray<TSharedPtr<FJsonValue>> DirtiedArray;
    for (const FString &Path : DirtiedPackages) {
      DirtiedArray.Add(MakeShared<FJsonValueString>(Path));
    }
    Resp->SetArrayField(TEXT("referencesNulled"), DirtiedArray);
    Resp->SetStringField(
        TEXT("forceHint"),
        TEXT("force:true replaced every in-memory pointer to the cleaned artifacts with "
             "null and dirtied the packages listed in referencesNulled. That is not "
             "transacted and cannot be undone - do NOT save them; asset.reload each one to "
             "discard the nulls."));
  }

  const int32 NotRemoved = Failed.Num() + Refused.Num();
  if (Cleaned.Num() > 0 && NotRemoved == 0) {
    Resp->SetBoolField(TEXT("success"), true);
    Ctx.SendSuccess(Resp);
  } else {
    FString Message = NotRemoved > 0
                          ? TEXT("Some animation artifacts could not be removed")
                          : TEXT("No animation artifacts were removed");
    FString ErrorCode =
        NotRemoved > 0 ? TEXT("CLEANUP_PARTIAL") : TEXT("CLEANUP_NO_OP");
    Resp->SetStringField(TEXT("error"), Message);
    Ctx.SendError(ErrorCode, Message);
  }
  return true;
}

// ============================================================
// animation.create_animation_bp - Create animation blueprint (compound-handler version)
// ============================================================
REGISTER_RPC_HANDLER("animation.create_animation_bp", "animation",
    "Create an Animation Blueprint asset bound to a Skeleton (or to the skeleton extracted from a SkeletalMesh). The recommended one-shot ABP creator: accepts meshPath or skeletonPath, defaults the parent class to UAnimInstance, and accepts a parentClass override. Idempotent: an existing UAnimBlueprint at the path is returned with existing:true, mode:\"updated_in_place\" and its AnimGraph untouched; ASSET_ALREADY_EXISTS when a different asset class occupies the path. For the authoring-path creator with an explicit save path use animation.authoring.create_anim_blueprint.",
    RPC_PARAMS(
        RPC_PARAM_REQ("name", "string", "Asset filename (no extension) for the new ABP."),
        RPC_PARAM_OPT("savePath", "path", "Content-browser folder; defaults to /Game/Animations."),
        RPC_PARAM_OPT("skeletonPath", "path", "Asset path to a Skeleton; required unless meshPath is provided."),
        RPC_PARAM_OPT("meshPath", "path", "SkeletalMesh asset path used to derive the Skeleton when skeletonPath is omitted."),
        RPC_PARAM_OPT("parentClass", "classref", "Path of an AnimInstance subclass to use as parent; defaults to /Script/Engine.AnimInstance."),
        RPC_PARAM_DEF("overwrite", "boolean", "Delete and recreate an existing ABP (discarding its AnimGraph) instead of returning it. Rejected with ASSET_IN_USE when any package still references it.", "false")
    ))
{
  FString Name = Ctx.GetString("name");
  if (Name.IsEmpty()) {
    Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, TEXT("name field required for animation blueprint creation"));
    return true;
  }

  FString SavePath = Ctx.GetString("savePath");
  if (SavePath.IsEmpty()) {
    SavePath = TEXT("/Game/Animations");
  }

  FString SkeletonPath = Ctx.GetString("skeletonPath");

  USkeleton *TargetSkeleton = nullptr;
  if (!SkeletonPath.IsEmpty()) {
    TargetSkeleton = LoadObject<USkeleton>(nullptr, *SkeletonPath);
  }

  // Fallback: try meshPath if skeleton missing
  if (!TargetSkeleton) {
    FString MeshPath = Ctx.GetString("meshPath");
    if (!MeshPath.IsEmpty()) {
      USkeletalMesh *Mesh = LoadObject<USkeletalMesh>(nullptr, *MeshPath);
      if (Mesh) {
        TargetSkeleton = Mesh->GetSkeleton();
      }
    }
  }

  if (!TargetSkeleton) {
    Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
                  TEXT("Valid skeletonPath or meshPath required to find skeleton"));
    return true;
  }

  // Nothing guarded this path before: an ABP already at SavePath/Name walked straight
  // into IAssetTools::CreateAsset -> CanCreateAsset and its three-modal chain.
  const AssetCreatePolicy::FResolution Resolution = AssetCreatePolicy::Resolve(
      FString::Printf(TEXT("%s/%s"), *SavePath, *Name), Name,
      UAnimBlueprint::StaticClass(), Ctx.GetBool(TEXT("overwrite"), false));
  if (Resolution.IsRejected()) {
    return AssetCreatePolicy::SendRejection(Ctx, Resolution);
  }
  if (Resolution.Action == AssetCreatePolicy::EAction::UpdateInPlace) {
    // Returning the existing ABP rather than rebuilding it keeps every AnimGraph node
    // and variable a previous run authored, and keeps its referencing Blueprints valid.
    TSharedPtr<FJsonObject> ExistingResp = MakeShared<FJsonObject>();
    ExistingResp->SetStringField(TEXT("blueprintPath"), Resolution.Existing->GetPathName());
    ExistingResp->SetStringField(TEXT("skeletonPath"), TargetSkeleton->GetPathName());
    AssetCreatePolicy::AddCreateReport(ExistingResp, Resolution);
    AddAssetVerification(ExistingResp, Resolution.Existing);
    Ctx.SendSuccess(ExistingResp);
    return true;
  }

  UAnimBlueprintFactory *Factory = NewObject<UAnimBlueprintFactory>();
  if (!Factory) {
    Ctx.SendError(ErrorCodes::ERR_FACTORY_FAILED, TEXT("Failed to create Animation Blueprint factory"));
    return true;
  }

  Factory->TargetSkeleton = TargetSkeleton;

  // Allow parent class override
  FString ParentClassPath = Ctx.GetString("parentClass");
  if (!ParentClassPath.IsEmpty()) {
    UClass *ParentClass = LoadClass<UObject>(nullptr, *ParentClassPath);
    if (ParentClass) {
      Factory->ParentClass = ParentClass;
    }
  }

  FAssetToolsModule &AssetToolsModule =
      FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools");
  UObject *NewAsset = AssetToolsModule.Get().CreateAsset(
      Name, SavePath, UAnimBlueprint::StaticClass(), Factory);

  if (NewAsset) {
    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetStringField(TEXT("blueprintPath"), NewAsset->GetPathName());
    Resp->SetStringField(TEXT("skeletonPath"), TargetSkeleton->GetPathName());
    AssetCreatePolicy::AddCreateReport(Resp, Resolution);
    AddAssetVerification(Resp, NewAsset);
    Ctx.SendSuccess(Resp);
  } else {
    Ctx.SendError(ErrorCodes::ERR_ASSET_CREATION_FAILED, TEXT("Failed to create Animation Blueprint asset"));
  }
  return true;
}

// ============================================================
// animation.create_blend_space - Create a 1D or 2D blend space
// ============================================================
REGISTER_RPC_HANDLER("animation.create_blend_space", "animation",
    "Create a Blend Space asset (1D or 2D) bound to a target Skeleton. Use animation.authoring.create_blend_space_1d / _2d for the typed equivalents; this single endpoint dispatches by the dimensions param. Idempotent: an existing blend space of the requested dimensionality has the axis bounds re-applied and is returned with existing:true, mode:\"updated_in_place\", keeping its samples; a dimensionality or class mismatch errors ASSET_ALREADY_EXISTS.",
    RPC_PARAMS(
        RPC_PARAM_REQ("name", "string", "Name for the blend space asset"),
        RPC_PARAM_REQ("skeletonPath", "path", "Path to skeleton asset"),
        RPC_PARAM_OPT("savePath", "path", "Save path (default /Game/Animations)"),
        RPC_PARAM_OPT("dimensions", "number", "1 or 2 (default 1)"),
        RPC_PARAM_OPT("minX", "number", "Axis 0 minimum value"),
        RPC_PARAM_OPT("maxX", "number", "Axis 0 maximum value"),
        RPC_PARAM_OPT("gridX", "number", "Axis 0 grid divisions"),
        RPC_PARAM_OPT("minY", "number", "Axis 1 minimum value (2D only)"),
        RPC_PARAM_OPT("maxY", "number", "Axis 1 maximum value (2D only)"),
        RPC_PARAM_OPT("gridY", "number", "Axis 1 grid divisions (2D only)"),
        RPC_PARAM_DEF("overwrite", "boolean", "Delete and recreate an existing blend space (discarding its samples) instead of re-applying the axis configuration to it. Rejected with ASSET_IN_USE when any package still references it.", "false")
    ))
{
  FString Name = Ctx.GetString("name");
  if (Name.IsEmpty()) {
    Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, TEXT("name field required for blend space creation"));
    return true;
  }

  FString SavePath = Ctx.GetString("savePath");
  if (SavePath.IsEmpty()) {
    SavePath = TEXT("/Game/Animations");
  }

  FString SkeletonPath = Ctx.GetString("skeletonPath");
  if (SkeletonPath.IsEmpty()) {
    Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
                  TEXT("skeletonPath is required to bind blend space to a skeleton"));
    return true;
  }

  USkeleton *TargetSkeleton = LoadObject<USkeleton>(nullptr, *SkeletonPath);
  if (!TargetSkeleton) {
    Ctx.SendError(ErrorCodes::ERR_LOAD_FAILED, TEXT("Failed to load skeleton for blend space"));
    return true;
  }

  int32 Dimensions = Ctx.GetInt("dimensions", 1);
  const bool bTwoDimensional = (Dimensions >= 2);

  // Validation
  double MinX = Ctx.GetNumber("minX", 0.0);
  double MaxX = Ctx.GetNumber("maxX", 1.0);
  double GridX = Ctx.GetNumber("gridX", 3.0);

  if (MinX >= MaxX) {
    Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, TEXT("minX must be less than maxX"));
    return true;
  }
  if (GridX <= 0) {
    Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, TEXT("gridX must be greater than 0"));
    return true;
  }

  if (bTwoDimensional) {
    double MinY = Ctx.GetNumber("minY", 0.0);
    double MaxY = Ctx.GetNumber("maxY", 1.0);
    double GridY = Ctx.GetNumber("gridY", 3.0);

    if (MinY >= MaxY) {
      Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, TEXT("minY must be less than maxY"));
      return true;
    }
    if (GridY <= 0) {
      Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, TEXT("gridY must be greater than 0"));
      return true;
    }
  }

#if MCP_HAS_BLENDSPACE_FACTORY
  // CreateBlendSpaceAsset goes through IAssetTools::CreateAsset, so an existing asset
  // at the path reached CanCreateAsset's modal chain before this guard.
  // bRequireExactClass is on because UBlendSpace1D DERIVES from UBlendSpace: an IsA
  // match would let a 2D request adopt an existing 1D asset and silently report it as
  // the requested 2D blend space.
  const AssetCreatePolicy::FResolution Resolution = AssetCreatePolicy::Resolve(
      FString::Printf(TEXT("%s/%s"), *SavePath, *Name), Name,
      bTwoDimensional ? UBlendSpace::StaticClass() : UBlendSpace1D::StaticClass(),
      Ctx.GetBool(TEXT("overwrite"), false), /*bRequireExactClass=*/true);
  if (Resolution.IsRejected()) {
    return AssetCreatePolicy::SendRejection(Ctx, Resolution);
  }
  if (Resolution.Action == AssetCreatePolicy::EAction::UpdateInPlace) {
    // Axis bounds are the only thing this verb configures, so re-applying them to the
    // existing asset IS the requested end state - and it keeps the samples that
    // animation.authoring.add_blend_sample already placed.
    const bool bExistingAxisConfigured = ApplyBlendSpaceConfiguration(
        Resolution.Existing, Ctx.GetRawPayload(), bTwoDimensional);

    TSharedPtr<FJsonObject> ExistingResp = MakeShared<FJsonObject>();
    ExistingResp->SetStringField(TEXT("blendSpacePath"), Resolution.Existing->GetPathName());
    ExistingResp->SetStringField(TEXT("skeletonPath"), SkeletonPath);
    ExistingResp->SetBoolField(TEXT("twoDimensional"), bTwoDimensional);
    ExistingResp->SetBoolField(TEXT("axisConfigured"), bExistingAxisConfigured);
    AssetCreatePolicy::AddCreateReport(ExistingResp, Resolution);
    AddAssetVerification(ExistingResp, Resolution.Existing);
    Ctx.SendSuccess(ExistingResp);
    return true;
  }

  FString FactoryError;
  UObject *CreatedBlendAsset = CreateBlendSpaceAsset(
      Name, SavePath, TargetSkeleton, bTwoDimensional, FactoryError);
  if (CreatedBlendAsset) {
    // ApplyBlendSpaceConfiguration already casts to UBlendSpace and returns false
    // (logging a Warning) for a non-UBlendSpace asset, so bAxisConfigured carries
    // the type-resolution result; no second cast / error path is needed here.
    const bool bAxisConfigured = ApplyBlendSpaceConfiguration(
        CreatedBlendAsset, Ctx.GetRawPayload(), bTwoDimensional);

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetStringField(TEXT("blendSpacePath"), CreatedBlendAsset->GetPathName());
    Resp->SetStringField(TEXT("skeletonPath"), SkeletonPath);
    Resp->SetBoolField(TEXT("twoDimensional"), bTwoDimensional);
    Resp->SetBoolField(TEXT("axisConfigured"), bAxisConfigured);
    AssetCreatePolicy::AddCreateReport(Resp, Resolution);
    if (!bAxisConfigured) {
      Resp->SetStringField(
          TEXT("warning"),
          TEXT("Axis configuration could not be applied to the created blend "
               "space."));
    }
    AddAssetVerification(Resp, CreatedBlendAsset);
    Ctx.SendSuccess(Resp);
  } else {
    FString Message = FactoryError.IsEmpty()
                          ? TEXT("Failed to create blend space asset")
                          : FactoryError;
    Ctx.SendError(ErrorCodes::ERR_ASSET_CREATION_FAILED, Message);
  }
#else
  Ctx.SendError(ErrorCodes::ERR_NOT_AVAILABLE,
                TEXT("Blend space creation requires editor blend space factories"));
#endif
  return true;
}

// ============================================================
// animation.create_state_machine - Create state machine in anim BP
// ============================================================
REGISTER_RPC_HANDLER("animation.create_state_machine", "animation",
    "Add a state machine sub-graph (with states and transitions) to an existing Animation Blueprint's AnimGraph. For finer-grained authoring use animation.authoring.add_state_machine / add_state / add_transition.",
    RPC_PARAMS(
        RPC_PARAM_REQ("blueprintPath", "path", "Asset path to an existing Animation Blueprint to host the state machine."),
        RPC_PARAM_OPT("machineName", "string", "Identifier of the new state machine node in the AnimGraph; defaults to 'StateMachine'."),
        RPC_PARAM_OPT_NESTED("states", "array", "Array of {name, isEntry} objects; one is auto-marked as entry if none is. Populate state graphs separately with animation.authoring graph verbs.", TEXT("name"), TEXT("isEntry")),
        RPC_PARAM_OPT("transitions", "array", "Array of {sourceState,targetState,condition} objects describing edges between named states.")
    ))
{
  FString BlueprintPath = Ctx.GetString("blueprintPath");
  if (BlueprintPath.IsEmpty()) {
    Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, TEXT("blueprintPath is required for create_state_machine"));
    return true;
  }

  FString MachineName = Ctx.GetString("machineName");
  if (MachineName.IsEmpty()) {
    MachineName = TEXT("StateMachine");
  }

#if MCP_ANIM_HANDLER_HAS_STATE_MACHINE_GRAPH
  UAnimBlueprint *AnimBP = Cast<UAnimBlueprint>(
      StaticLoadObject(UAnimBlueprint::StaticClass(), nullptr, *BlueprintPath));
  if (!AnimBP) {
    Ctx.SendError(ErrorCodes::ERR_ANIM_BP_NOT_FOUND,
                  FString::Printf(TEXT("Could not load animation blueprint: %s"),
                                  *BlueprintPath));
    return true;
  }

  UEdGraph *AnimGraph =
      AnimGraphConstructionUtils::GetAnimGraphFromBlueprint(AnimBP);
  if (!AnimGraph) {
    Ctx.SendError(ErrorCodes::ERR_GRAPH_NOT_FOUND,
                  TEXT("Could not find AnimGraph in blueprint"));
    return true;
  }

  // Parse every nested element before the first graph mutation. In particular,
  // transition endpoints must be resolved against the complete state-name set so
  // a late missing endpoint cannot leave a partially-created state machine behind.
  TArray<TSharedPtr<FJsonObject>> RequestedStates;
  TSet<FString> RequestedStateNames;
  int32 StateCount = 0;
  FString EntryStateName;
  FString FirstStateName;
  const TArray<TSharedPtr<FJsonValue>> *StatesArray = Ctx.GetArray("states");
  if (StatesArray) {
    RequestedStates.Reserve(StatesArray->Num());
    for (int32 StateIndex = 0; StateIndex < StatesArray->Num(); ++StateIndex) {
      const TSharedPtr<FJsonValue> &StateValue = (*StatesArray)[StateIndex];
      if (!StateValue.IsValid() || StateValue->Type != EJson::Object ||
          !StateValue->AsObject().IsValid()) {
        continue;
      }

      const TSharedPtr<FJsonObject> StateObj = StateValue->AsObject();
      FString StateName;
      if (!StateObj->TryGetStringField(TEXT("name"), StateName) ||
          StateName.IsEmpty()) {
        continue;
      }

      RequestedStates.Add(StateObj);
      RequestedStateNames.Add(StateName);
    }
  }

  struct FRequestedTransition {
    FString SourceState;
    FString TargetState;
    bool bConditionRequested = false;
  };

  TArray<FRequestedTransition> RequestedTransitions;
  const TArray<TSharedPtr<FJsonValue>> *TransitionsArray =
      Ctx.GetArray("transitions");
  if (TransitionsArray) {
    RequestedTransitions.Reserve(TransitionsArray->Num());
    for (int32 TransitionIndex = 0;
         TransitionIndex < TransitionsArray->Num(); ++TransitionIndex) {
      const TSharedPtr<FJsonValue> &TransitionValue =
          (*TransitionsArray)[TransitionIndex];
      if (!TransitionValue.IsValid() ||
          TransitionValue->Type != EJson::Object ||
          !TransitionValue->AsObject().IsValid()) {
        continue;
      }

      const TSharedPtr<FJsonObject> TransitionObj =
          TransitionValue->AsObject();
      FRequestedTransition RequestedTransition;
      if (!TransitionObj->TryGetStringField(TEXT("sourceState"),
                                            RequestedTransition.SourceState) ||
          RequestedTransition.SourceState.IsEmpty()) {
        continue;
      }
      if (!TransitionObj->TryGetStringField(TEXT("targetState"),
                                            RequestedTransition.TargetState) ||
          RequestedTransition.TargetState.IsEmpty()) {
        continue;
      }

      if (!RequestedStateNames.Contains(RequestedTransition.SourceState)) {
        Ctx.SendError(
            ErrorCodes::ERR_SOURCE_STATE_NOT_FOUND,
            FString::Printf(TEXT("Source state '%s' not found"),
                            *RequestedTransition.SourceState));
        return true;
      }
      if (!RequestedStateNames.Contains(RequestedTransition.TargetState)) {
        Ctx.SendError(
            ErrorCodes::ERR_TARGET_STATE_NOT_FOUND,
            FString::Printf(TEXT("Target state '%s' not found"),
                            *RequestedTransition.TargetState));
        return true;
      }

      FString Condition;
      RequestedTransition.bConditionRequested =
          TransitionObj->TryGetStringField(TEXT("condition"), Condition) &&
          !Condition.IsEmpty();
      RequestedTransitions.Add(MoveTemp(RequestedTransition));
    }
  }

  const BlueprintGraphSnapshot::FBlueprintGraphSnapshot GraphSnapshot =
      BlueprintGraphSnapshot::Capture(AnimBP);
  UPackage *Package = AnimBP->GetOutermost();
  const bool bPackageWasDirty = Package && Package->IsDirty();
  bool bConditionRequested = false;
  int32 TransitionCount = 0;

  // Create and populate the graph transactionally. The transaction restores
  // property-level diffs, while the graph snapshot removes NewObject/CreateNewGraph
  // additions that bypass the transaction buffer.
  {
    FScopedTransaction Transaction(
        NSLOCTEXT("AnimationHandler", "CreateStateMachine",
                  "Create Animation State Machine"));
    PinWrightTransactionUtils::PrepareTransactionalSnapshot(AnimBP);

    auto RollbackFailure = [&Transaction, &GraphSnapshot, Package,
                            bPackageWasDirty]()
    {
      PinWrightTransactionUtils::ApplyAndCancelTransaction(Transaction);
      BlueprintGraphSnapshot::RollbackToSnapshot(GraphSnapshot);
      if (Package && !bPackageWasDirty)
      {
        Package->SetDirtyFlag(false);
      }
    };

    // Create the state machine node (direct graph manipulation, not a console
    // verb). This is the same construction path as animation.authoring.add_state_machine.
    UAnimGraphNode_StateMachine *SMNode =
        AnimGraphConstructionUtils::CreateStateMachine(
            AnimBP, AnimGraph, FName(*MachineName), FVector2D(0.0, 0.0));
    if (!SMNode || !SMNode->EditorStateMachineGraph)
    {
      RollbackFailure();
      Ctx.SendError(
          ErrorCodes::ERR_STATE_MACHINE_CREATE_FAILED,
          FString::Printf(TEXT("Failed to create state machine '%s'"), *MachineName));
      return true;
    }

    UAnimationStateMachineGraph *SMGraph =
        Cast<UAnimationStateMachineGraph>(SMNode->EditorStateMachineGraph);
    if (!SMGraph)
    {
      RollbackFailure();
      Ctx.SendError(ErrorCodes::ERR_INVALID_GRAPH,
                    TEXT("Invalid state machine graph"));
      return true;
    }

    // Add each requested state, laying them out left-to-right so they do not stack
    // on top of each other at the origin. Remember the state flagged isEntry so the
    // state machine's entry node can be rewired to it after all states exist, and
    // remember the first successfully-created state as the default-entry fallback.
    for (const TSharedPtr<FJsonObject> &StateObj : RequestedStates)
    {
      FString StateName;
      StateObj->TryGetStringField(TEXT("name"), StateName);

      const FVector2D StatePos(300.0 + StateCount * 250.0, 0.0);
      UAnimStateNode *StateNode = nullptr;
#if WITH_DEV_AUTOMATION_TESTS
      if (!AnimationHandlerTestHooks::ShouldFailCreateState(StateCount))
#endif
      {
        StateNode = AnimGraphConstructionUtils::CreateState(
            SMGraph, FName(*StateName), StatePos);
      }
      if (!StateNode)
      {
        RollbackFailure();
        Ctx.SendError(
            ErrorCodes::ERR_STATE_CREATE_FAILED,
            FString::Printf(TEXT("Failed to create state '%s'"), *StateName));
        return true;
      }
      ++StateCount;
      if (FirstStateName.IsEmpty())
      {
        FirstStateName = StateName;
      }

      bool bIsEntry = false;
      StateObj->TryGetBoolField(TEXT("isEntry"), bIsEntry);
      if (bIsEntry && EntryStateName.IsEmpty())
      {
        EntryStateName = StateName;
      }
    }

    // If no state was explicitly flagged isEntry, default the entry to the first
    // state added (matches the documented "one is auto-marked as entry if none is").
    if (EntryStateName.IsEmpty())
    {
      EntryStateName = FirstStateName;
    }

    // Rewire the entry node to the chosen entry state. Preserve the convenience
    // verb's existing best-effort contract: states/transitions remain valid even
    // when the engine refuses this optional entry connection.
    if (!EntryStateName.IsEmpty())
    {
      if (UAnimStateNode *EntryState =
              AnimGraphConstructionUtils::FindStateNode(SMGraph, EntryStateName))
      {
        FString EntryErrorCode;
        FString EntryErrorMessage;
        AnimGraphConstructionUtils::SetStateMachineEntry(
            SMGraph, EntryState, EntryErrorCode, EntryErrorMessage);
      }
    }

    // Add each requested transition between named states. The inline `condition`
    // rule string is NOT authored here — transition rule bodies are a deferred
    // capability; creating the transition edge gives a usable always-true
    // transition the caller can fill in.
    for (const FRequestedTransition &RequestedTransition : RequestedTransitions)
    {
      UAnimStateNode *FromNode = AnimGraphConstructionUtils::FindStateNode(
          SMGraph, RequestedTransition.SourceState);
      UAnimStateNode *ToNode = AnimGraphConstructionUtils::FindStateNode(
          SMGraph, RequestedTransition.TargetState);
      if (!FromNode)
      {
        RollbackFailure();
        Ctx.SendError(
            ErrorCodes::ERR_SOURCE_STATE_NOT_FOUND,
            FString::Printf(TEXT("Source state '%s' not found"),
                            *RequestedTransition.SourceState));
        return true;
      }
      if (!ToNode)
      {
        RollbackFailure();
        Ctx.SendError(
            ErrorCodes::ERR_TARGET_STATE_NOT_FOUND,
            FString::Printf(TEXT("Target state '%s' not found"),
                            *RequestedTransition.TargetState));
        return true;
      }

      UAnimStateTransitionNode *TransNode =
          AnimGraphConstructionUtils::CreateTransition(FromNode, ToNode,
                                                       FVector2D::ZeroVector);
      if (!TransNode)
      {
        RollbackFailure();
        Ctx.SendError(
            ErrorCodes::ERR_TRANSITION_CREATE_FAILED,
            FString::Printf(
                TEXT("Failed to create transition from '%s' to '%s'"),
                *RequestedTransition.SourceState,
                *RequestedTransition.TargetState));
        return true;
      }
      ++TransitionCount;
      bConditionRequested |= RequestedTransition.bConditionRequested;
    }

    // Verify the complete result before marking the blueprint structurally
    // modified. A successful factory call is not sufficient if the graph did not
    // retain every requested node.
    if (!AnimGraph->Nodes.Contains(SMNode))
    {
      RollbackFailure();
      Ctx.SendError(
          ErrorCodes::ERR_STATE_MACHINE_CREATE_FAILED,
          FString::Printf(TEXT("State machine '%s' was not retained in the AnimGraph"),
                          *MachineName));
      return true;
    }

    for (const TSharedPtr<FJsonObject> &StateObj : RequestedStates)
    {
      FString StateName;
      StateObj->TryGetStringField(TEXT("name"), StateName);
      if (!AnimGraphConstructionUtils::FindStateNode(SMGraph, StateName))
      {
        RollbackFailure();
        Ctx.SendError(
            ErrorCodes::ERR_STATE_MACHINE_CREATE_FAILED,
            FString::Printf(TEXT("State machine verification could not find state '%s'"),
                            *StateName));
        return true;
      }
    }

    int32 ActualStateCount = 0;
    int32 ActualTransitionCount = 0;
    for (UEdGraphNode *Node : SMGraph->Nodes)
    {
      if (Cast<UAnimStateNode>(Node))
      {
        ++ActualStateCount;
      }
      if (Cast<UAnimStateTransitionNode>(Node))
      {
        ++ActualTransitionCount;
      }
    }
    if (ActualStateCount != StateCount ||
        ActualTransitionCount != TransitionCount)
    {
      RollbackFailure();
      Ctx.SendError(
          ErrorCodes::ERR_STATE_MACHINE_CREATE_FAILED,
          FString::Printf(
              TEXT("State machine verification failed: expected %d state(s) and %d transition(s), found %d and %d"),
              StateCount, TransitionCount, ActualStateCount,
              ActualTransitionCount));
      return true;
    }

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(AnimBP);
  }

  TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
  Resp->SetStringField(TEXT("blueprintPath"), BlueprintPath);
  Resp->SetStringField(TEXT("machineName"), MachineName);
  Resp->SetNumberField(TEXT("statesCreated"), StateCount);
  Resp->SetNumberField(TEXT("transitionsCreated"), TransitionCount);
  if (bConditionRequested) {
    // Be honest that the convenience method does not author rule bodies, rather
    // than silently dropping the `condition` strings.
    Resp->SetStringField(
        TEXT("warning"),
        TEXT("Inline transition 'condition' strings are not authored; the "
             "transitions were created as always-enterable. Author rule bodies "
             "via the blueprint graph or a dedicated transition-rule RPC."));
  }
  Ctx.SendSuccess(Resp);
#else
  Ctx.SendError(
      ErrorCodes::ERR_ANIMGRAPH_MODULE_UNAVAILABLE,
      FString::Printf(
          TEXT("Cannot create state machine '%s': AnimGraph state-machine module "
               "headers not available in this build."),
          *MachineName));
#endif
  return true;
}

// ============================================================
// animation.create_animation_asset - Create AnimSequence or AnimMontage
// ============================================================
REGISTER_RPC_HANDLER("animation.create_animation_asset", "animation",
    "Generic creator that produces an empty AnimSequence or AnimMontage bound to a Skeleton. Idempotent: an existing asset of the requested type is returned with existing:true, mode:\"updated_in_place\" (and the legacy existingAsset:true), while a type mismatch errors ASSET_ALREADY_EXISTS. For type-specific authoring with full track/section control use animation.authoring.create_animation_sequence or animation.authoring.create_montage.",
    RPC_PARAMS(
        RPC_PARAM_REQ("name", "string", "Asset filename (no extension) for the new animation asset."),
        RPC_PARAM_REQ("skeletonPath", "path", "Asset path to the Skeleton the new asset will be bound to."),
        RPC_PARAM_OPT("savePath", "path", "Content-browser folder; defaults to /Game/Animations."),
        RPC_PARAM_OPT("assetType", "string", "'sequence' (default) creates an AnimSequence; 'montage' creates an AnimMontage."),
        RPC_PARAM_DEF("overwrite", "boolean", "Delete and recreate an existing animation asset instead of returning it. Rejected with ASSET_IN_USE when any package still references it.", "false")
    ))
{
  FString AssetName = Ctx.GetString("name");
  if (AssetName.IsEmpty()) {
    Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, TEXT("name required for create_animation_asset"));
    return true;
  }

  FString SavePath = Ctx.GetString("savePath");
  if (SavePath.IsEmpty()) {
    SavePath = TEXT("/Game/Animations");
  }
  SavePath = SavePath.TrimStartAndEnd();

  if (!FPackageName::IsValidLongPackageName(SavePath)) {
    FString NormalizedPath;
    if (!FPackageName::TryConvertFilenameToLongPackageName(SavePath, NormalizedPath)) {
      Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, TEXT("Invalid savePath for animation asset"));
      return true;
    }
    SavePath = NormalizedPath;
  }

  if (PinWrightPieState::IsPlayInEditorActive()) {
    Ctx.SendError(
        ErrorCodes::ERR_PIE_ACTIVE,
        TEXT("animation.create_animation_asset cannot run while the editor is in play mode; stop PIE and retry."));
    return true;
  }

  FString SkeletonPath = Ctx.GetString("skeletonPath");
  USkeleton *TargetSkeleton = nullptr;
  const bool bHadSkeletonPath = !SkeletonPath.IsEmpty();
  if (bHadSkeletonPath) {
    TargetSkeleton = Cast<USkeleton>(ResolveAsset(SkeletonPath, /*bLoadObject=*/true).Object);
  }

  if (!TargetSkeleton) {
    if (bHadSkeletonPath) {
      Ctx.SendError(ErrorCodes::ERR_ASSET_NOT_FOUND,
                    FString::Printf(TEXT("Skeleton not found: %s"), *SkeletonPath));
    } else {
      Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
                    TEXT("skeletonPath is required for create_animation_asset"));
    }
    return true;
  }

  if (!DoesAssetDirectoryExist(SavePath)) {
    UEditorAssetLibrary::MakeDirectory(SavePath);
  }

  FString AssetType = Ctx.GetString("assetType").ToLower();
  if (AssetType.IsEmpty()) {
    AssetType = TEXT("sequence");
  }

  UFactory *Factory = nullptr;
  UClass *DesiredClass = nullptr;
  FString AssetTypeString;

  if (AssetType == TEXT("montage")) {
    UAnimMontageFactory *MontageFactory = NewObject<UAnimMontageFactory>();
    if (MontageFactory) {
      MontageFactory->TargetSkeleton = TargetSkeleton;
      Factory = MontageFactory;
      DesiredClass = UAnimMontage::StaticClass();
      AssetTypeString = TEXT("Montage");
    }
  } else {
    UAnimSequenceFactory *SequenceFactory = NewObject<UAnimSequenceFactory>();
    if (SequenceFactory) {
      SequenceFactory->TargetSkeleton = TargetSkeleton;
      Factory = SequenceFactory;
      DesiredClass = UAnimSequence::StaticClass();
      AssetTypeString = TEXT("Sequence");
    }
  }

  if (!Factory || !DesiredClass) {
    Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
                  TEXT("Unsupported assetType for create_animation_asset"));
    return true;
  }

  const FString ObjectPath = FString::Printf(TEXT("%s/%s"), *SavePath, *AssetName);

  // Replaces the registry-only DoesAssetExist branch. It already returned the existing
  // asset, but it missed a same-session in-memory one (which then reached
  // CanCreateAsset's modal chain) and could not tell a montage from a sequence.
  const AssetCreatePolicy::FResolution Resolution = AssetCreatePolicy::Resolve(
      ObjectPath, AssetName, DesiredClass, Ctx.GetBool(TEXT("overwrite"), false));
  if (Resolution.IsRejected()) {
    return AssetCreatePolicy::SendRejection(Ctx, Resolution);
  }
  if (Resolution.Action == AssetCreatePolicy::EAction::UpdateInPlace) {
    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetStringField(TEXT("assetPath"), Resolution.Existing->GetPathName());
    Resp->SetStringField(TEXT("assetType"), AssetTypeString);
    // Retained alongside the policy's existing/mode pair: callers already read it.
    Resp->SetBoolField(TEXT("existingAsset"), true);
    AssetCreatePolicy::AddCreateReport(Resp, Resolution);
    AddAssetVerification(Resp, Resolution.Existing);
    Ctx.SendSuccess(Resp);
    return true;
  }

  FAssetToolsModule &AssetToolsModule =
      FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools");
  UObject *NewAsset = AssetToolsModule.Get().CreateAsset(
      AssetName, SavePath, DesiredClass, Factory);

  if (!NewAsset) {
    Ctx.SendError(ErrorCodes::ERR_ASSET_CREATION_FAILED, TEXT("Failed to create animation asset"));
  } else {
    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetStringField(TEXT("assetPath"), NewAsset->GetPathName());
    Resp->SetStringField(TEXT("assetType"), AssetTypeString);
    Resp->SetBoolField(TEXT("existingAsset"), false);
    AssetCreatePolicy::AddCreateReport(Resp, Resolution);
    AddAssetVerification(Resp, NewAsset);
    Ctx.SendSuccess(Resp);
  }
  return true;
}

// ============================================================
// animation.setup_retargeting - Duplicate animations and assign a new skeleton
// ============================================================
REGISTER_RPC_HANDLER("animation.setup_retargeting", "animation",
    "Duplicate AnimSequence assets and assign the target Skeleton pointer to each copy. This does not remap animation tracks and reports retargeted=false. For real IK retargeting, configure an IK Rig pair and run Unreal's IK Retargeter batch export.",
    RPC_PARAMS(
        RPC_PARAM_REQ("sourceSkeleton", "path", "Path to source skeleton"),
        RPC_PARAM_REQ("targetSkeleton", "path", "Path to target skeleton"),
        RPC_PARAM_OPT("assets", "array", "Array of AnimSequence paths to duplicate"),
        RPC_PARAM_OPT("savePath", "path", "Save directory for duplicated assets; omit to use each source directory"),
        RPC_PARAM_OPT("suffix", "string", "Suffix for duplicated asset names (legacy default _Retargeted)"),
        RPC_PARAM_OPT("overwrite", "boolean", "Replace an existing destination from a validated staged source copy")
    ))
{
  const TSharedPtr<FJsonObject> &RawPayload = Ctx.GetRawPayload();
  const bool bSavePathProvided =
      RawPayload.IsValid() && RawPayload->HasField(TEXT("savePath"));
  const FString RequestedSavePath = Ctx.GetString(TEXT("savePath"));
  FString SavePath = RequestedSavePath.TrimStartAndEnd();
  if (bSavePathProvided) {
    const bool bLooksLikeObjectPath =
        SavePath.StartsWith(TEXT("/")) && SavePath.Contains(TEXT("."));
    const bool bIsWritableLongPackageName =
        !FPackageName::GetPackageMountPoint(SavePath).IsNone() &&
        FPackageName::IsValidLongPackageName(SavePath);
    if (bLooksLikeObjectPath || !bIsWritableLongPackageName) {
      FString NormalizedPath;
      if (!bLooksLikeObjectPath && !SavePath.IsEmpty() &&
          FPackageName::TryConvertFilenameToLongPackageName(
              SavePath, NormalizedPath) &&
          FPackageName::IsValidLongPackageName(NormalizedPath)) {
        SavePath = NormalizedPath;
      } else {
        Ctx.SendError(
            ErrorCodes::ERR_INVALID_PATH,
            FString::Printf(
                TEXT("savePath '%s' is not a writable long package directory. "
                     "Omit savePath to use each source directory."),
                *RequestedSavePath));
        return true;
      }
    }
  }

  if (PinWrightPieState::IsPlayInEditorActive()) {
    Ctx.SendError(
        ErrorCodes::ERR_PIE_ACTIVE,
        TEXT("animation.setup_retargeting cannot run while the editor is in play mode; stop PIE and retry."));
    return true;
  }

  FString SourceSkeletonPath = Ctx.GetString("sourceSkeleton");
  FString TargetSkeletonPath = Ctx.GetString("targetSkeleton");

  USkeleton *SourceSkeleton = nullptr;
  USkeleton *TargetSkeleton = nullptr;

  if (!SourceSkeletonPath.IsEmpty()) {
    SourceSkeleton = LoadObject<USkeleton>(nullptr, *SourceSkeletonPath);
  }
  if (!TargetSkeletonPath.IsEmpty()) {
    TargetSkeleton = LoadObject<USkeleton>(nullptr, *TargetSkeletonPath);
  }

  if (!SourceSkeleton || !TargetSkeleton) {
    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetStringField(TEXT("sourceSkeleton"), SourceSkeletonPath);
    Resp->SetStringField(TEXT("targetSkeleton"), TargetSkeletonPath);
    Ctx.SendError(ErrorCodes::ERR_ASSET_NOT_FOUND,
                  TEXT("Retargeting failed - source or target skeleton not found"));
    return true;
  }

  const TArray<TSharedPtr<FJsonValue>> *AssetsArray = Ctx.GetArray("assets");

  FString Suffix = Ctx.GetString("suffix");
  if (Suffix.IsEmpty()) {
    Suffix = TEXT("_Retargeted");
  }

  bool bOverwrite = Ctx.GetBool("overwrite", false);

  TArray<FString> DuplicatedAssets;
  TArray<FString> SkippedAssets;
  TArray<TSharedPtr<FJsonValue>> WarningArray;

  const auto RenameWithoutRedirector = [](UObject *Asset,
                                           const FString &PackageName,
                                           const FString &AssetName,
                                           FString &OutError) {
    ObjectTools::FPackageGroupName Target;
    Target.PackageName = PackageName;
    Target.GroupName = TEXT("");
    Target.ObjectName = AssetName;
    TSet<UPackage *> PackagesUserRefusedToFullyLoad;
    FText RenameError;
    const bool bRenamed = ObjectTools::RenameSingleObject(
        Asset, Target, PackagesUserRefusedToFullyLoad, RenameError, nullptr,
        /*bLeaveRedirector=*/false);
    OutError = RenameError.ToString();
    return bRenamed;
  };

  if (AssetsArray && AssetsArray->Num() > 0) {
    for (const TSharedPtr<FJsonValue> &Value : *AssetsArray) {
      if (!Value.IsValid() || Value->Type != EJson::String) {
        continue;
      }

      const FString SourceAssetPath = Value->AsString();
      UAnimSequence *SourceSequence =
          LoadObject<UAnimSequence>(nullptr, *SourceAssetPath);
      if (!SourceSequence) {
        WarningArray.Add(MakeShared<FJsonValueString>(FString::Printf(
            TEXT("Skipped non-sequence asset: %s"), *SourceAssetPath)));
        SkippedAssets.Add(SourceAssetPath);
        continue;
      }

      FString DestinationFolder = SavePath;
      if (DestinationFolder.IsEmpty()) {
        const FString SourcePackageName =
            SourceSequence->GetOutermost()->GetName();
        DestinationFolder =
            FPackageName::GetLongPackagePath(SourcePackageName);
      }

      if (!DestinationFolder.IsEmpty() &&
          !DoesAssetDirectoryExist(DestinationFolder)) {
        UEditorAssetLibrary::MakeDirectory(DestinationFolder);
      }

      FString DestinationAssetName = FPackageName::GetShortName(
          SourceSequence->GetOutermost()->GetName());
      DestinationAssetName += Suffix;

      const FString DestinationObjectPath = FString::Printf(
          TEXT("%s/%s"), *DestinationFolder, *DestinationAssetName);

      const FResolvedAsset DestinationResolution =
          ResolveAsset(DestinationObjectPath, /*bLoadObject=*/true);
      const bool bDestinationExists = DestinationResolution.bExists;
      if (bDestinationExists && !bOverwrite) {
        WarningArray.Add(MakeShared<FJsonValueString>(FString::Printf(
            TEXT("Destination already exists, skipping: %s"),
            *DestinationObjectPath)));
        SkippedAssets.Add(SourceAssetPath);
        continue;
      }

      UAnimSequence *ExistingDestination = nullptr;
      if (bDestinationExists) {
        ExistingDestination = Cast<UAnimSequence>(DestinationResolution.Object);
        if (!ExistingDestination) {
          WarningArray.Add(MakeShared<FJsonValueString>(FString::Printf(
              TEXT("Destination exists but is not an AnimSequence: %s"),
              *DestinationObjectPath)));
          SkippedAssets.Add(SourceAssetPath);
          continue;
        }
      }

      const FString StagedAssetPath = bDestinationExists
          ? FString::Printf(TEXT("%s/__PW_RetargetStage_%s_%s"),
                *DestinationFolder, *DestinationAssetName,
                *FGuid::NewGuid().ToString(EGuidFormats::Digits))
          : DestinationObjectPath;
      UAnimSequence *StagedSequence = Cast<UAnimSequence>(
          UEditorAssetLibrary::DuplicateAsset(SourceAssetPath, StagedAssetPath));
      if (!StagedSequence) {
        WarningArray.Add(MakeShared<FJsonValueString>(FString::Printf(
            TEXT("Failed to duplicate asset: %s"), *SourceAssetPath)));
        SkippedAssets.Add(SourceAssetPath);
        continue;
      }

      StagedSequence->Modify();
      // DuplicateAsset carries the source FK ControlRig binding; rebind it before changing the
      // sequence skeleton so AnimationData validation sees the target skeleton.
      // Through UE 5.7 UAnimSequencerController::UpdateWithSkeleton opens its bracket with the
      // caller's bShouldTransact but closes it with the hardcoded default, so a non-transacting
      // call reaches FChangeTransactor::CloseTransaction with no open transaction and asserts.
      // An outer non-transacting bracket keeps that inner close off bracket depth zero, the only
      // depth at which the transactor is touched.
      {
        IAnimationDataController::FScopedBracket SkeletonBracket(
            StagedSequence->GetController(),
            FText::FromString(TEXT("Retarget stage skeleton update")),
            /*bShouldTransact=*/false);
        StagedSequence->GetController().UpdateWithSkeleton(TargetSkeleton, false);
      }
      StagedSequence->SetSkeleton(TargetSkeleton);
      McpSafeAssetSave(StagedSequence);

      const IAnimationDataModel *SourceModel = SourceSequence->GetDataModel();
      const IAnimationDataModel *StagedModel = StagedSequence->GetDataModel();
      const bool bStageValidated =
          StagedSequence->GetSkeleton() == TargetSkeleton &&
          SourceModel && StagedModel &&
          SourceModel->GenerateGuid() == StagedModel->GenerateGuid();
      if (!bStageValidated) {
        UEditorAssetLibrary::DeleteAsset(StagedAssetPath);
        WarningArray.Add(MakeShared<FJsonValueString>(
            FString::Printf(TEXT("Staged copy failed skeleton/track validation: %s"),
                            *SourceAssetPath)));
        SkippedAssets.Add(SourceAssetPath);
        continue;
      }

      EAssetSaveState StagedSaveState = EAssetSaveState::NotRequested;
      if (!SaveAssetToDiskReportingPresence(
              StagedSequence, /*bForce=*/true, nullptr, nullptr,
              &StagedSaveState)) {
        const bool bOriginalFilePresent = ExistingDestination &&
            FPackageName::DoesPackageExist(DestinationObjectPath);
        TSharedPtr<FJsonObject> Failure = MakeShared<FJsonObject>();
        Failure->SetStringField(TEXT("failedPhase"), TEXT("save_stage"));
        Failure->SetStringField(TEXT("stagedAsset"), StagedAssetPath);
        Failure->SetStringField(TEXT("destinationAsset"), DestinationObjectPath);
        Failure->SetBoolField(TEXT("originalRestored"), bOriginalFilePresent);
        Failure->SetBoolField(TEXT("originalFilePresent"), bOriginalFilePresent);
        Failure->SetBoolField(TEXT("diskPersistenceGuaranteed"),
                              bOriginalFilePresent);
        AddAssetSaveReport(Failure, true, false, StagedSaveState);
        UEditorAssetLibrary::DeleteAsset(StagedAssetPath);
        Ctx.SendError(
            ErrorCodes::ERR_SAVE_FAILED,
            FString::Printf(
                TEXT("Could not persist staged replacement '%s'; destination was not touched"),
                *StagedAssetPath),
            Failure);
        return true;
      }

      UAnimSequence *DestinationSequence = StagedSequence;
      if (ExistingDestination) {
        const FString BackupAssetPath = FString::Printf(
            TEXT("%s/__PW_RetargetBackup_%s_%s"), *DestinationFolder,
            *DestinationAssetName,
            *FGuid::NewGuid().ToString(EGuidFormats::Digits));
        const FString BackupAssetName =
            FPackageName::GetLongPackageAssetName(BackupAssetPath);

        FString RenameError;
        if (!RenameWithoutRedirector(ExistingDestination, BackupAssetPath,
                                     BackupAssetName, RenameError)) {
          UEditorAssetLibrary::DeleteAsset(StagedAssetPath);
          TSharedPtr<FJsonObject> Failure = MakeShared<FJsonObject>();
          Failure->SetStringField(TEXT("failedPhase"), TEXT("rename_original_to_backup"));
          Failure->SetStringField(TEXT("destinationAsset"), DestinationObjectPath);
          Failure->SetBoolField(TEXT("originalRestored"), true);
          Failure->SetBoolField(TEXT("originalFilePresent"),
                                FPackageName::DoesPackageExist(DestinationObjectPath));
          Failure->SetBoolField(TEXT("diskPersistenceGuaranteed"), true);
          Ctx.SendError(
              ErrorCodes::ERR_RENAME_FAILED,
              FString::Printf(TEXT("Could not stage the existing destination for overwrite: %s"),
                              *RenameError),
              Failure);
          return true;
        }

        UAnimSequence *BackupSequence = ExistingDestination;
        TArray<UObject *> ExistingAssets = {ExistingDestination};
        ObjectTools::ForceReplaceReferences(StagedSequence, ExistingAssets);

        if (!RenameWithoutRedirector(StagedSequence, DestinationObjectPath,
                                     DestinationAssetName, RenameError)) {
          TArray<UObject *> StagedAssets = {StagedSequence};
          ObjectTools::ForceReplaceReferences(BackupSequence, StagedAssets);
          FString RestoreError;
          const bool bRestored = RenameWithoutRedirector(
              BackupSequence, DestinationObjectPath, DestinationAssetName,
              RestoreError);
          UEditorAssetLibrary::DeleteAsset(StagedAssetPath);
          const bool bOriginalFilePresent =
              FPackageName::DoesPackageExist(DestinationObjectPath);
          TSharedPtr<FJsonObject> Failure = MakeShared<FJsonObject>();
          Failure->SetStringField(TEXT("failedPhase"), TEXT("rename_stage_to_destination"));
          Failure->SetStringField(TEXT("destinationAsset"), DestinationObjectPath);
          Failure->SetBoolField(TEXT("originalRestored"), bRestored);
          Failure->SetBoolField(TEXT("originalFilePresent"), bOriginalFilePresent);
          Failure->SetBoolField(TEXT("diskPersistenceGuaranteed"),
                                bRestored && bOriginalFilePresent);
          Ctx.SendError(
              ErrorCodes::ERR_RENAME_FAILED,
              FString::Printf(
                  TEXT("Could not publish staged overwrite: %s; restore: %s"),
                  *RenameError, bRestored ? TEXT("restored") : *RestoreError),
              Failure);
          return true;
        }

        DestinationSequence = StagedSequence;
        FString DestinationFilename;
        const bool bHasDestinationFilename =
            FPackageName::TryConvertLongPackageNameToFilename(
                DestinationObjectPath, DestinationFilename,
                FPackageName::GetAssetPackageExtension());
        TArray<uint8> DestinationBytesBefore;
        const bool bDestinationBytesBeforeReadable =
            bHasDestinationFilename &&
            FFileHelper::LoadFileToArray(DestinationBytesBefore,
                                         *DestinationFilename);
        EAssetSaveState DestinationSaveState = EAssetSaveState::NotRequested;
        bool bDestinationSaved = SaveAssetToDiskReportingPresence(
            DestinationSequence, /*bForce=*/true, nullptr, nullptr,
            &DestinationSaveState,
            /*bAllowDivergedOverwrite=*/true);
        if (!bDestinationSaved && bDestinationBytesBeforeReadable) {
          TArray<uint8> DestinationBytesAfter;
          bDestinationSaved =
              FFileHelper::LoadFileToArray(DestinationBytesAfter,
                                           *DestinationFilename) &&
              DestinationBytesAfter != DestinationBytesBefore;
        }
        if (!bDestinationSaved) {
          TArray<UObject *> StagedAssets = {StagedSequence};
          ObjectTools::ForceReplaceReferences(BackupSequence, StagedAssets);
          FString StageRestoreError;
          const bool bStageMovedAside = RenameWithoutRedirector(
              StagedSequence, StagedAssetPath,
              FPackageName::GetLongPackageAssetName(StagedAssetPath),
              StageRestoreError);
          FString OriginalRestoreError;
          const bool bOriginalRestored = RenameWithoutRedirector(
              BackupSequence, DestinationObjectPath, DestinationAssetName,
              OriginalRestoreError);
          const bool bOriginalFilePresent =
              FPackageName::DoesPackageExist(DestinationObjectPath);
          TSharedPtr<FJsonObject> Failure = MakeShared<FJsonObject>();
          Failure->SetStringField(TEXT("failedPhase"), TEXT("save_destination"));
          Failure->SetStringField(TEXT("destinationAsset"), DestinationObjectPath);
          Failure->SetBoolField(TEXT("stageMovedAside"), bStageMovedAside);
          Failure->SetBoolField(TEXT("originalRestored"), bOriginalRestored);
          Failure->SetBoolField(TEXT("originalFilePresent"), bOriginalFilePresent);
          Failure->SetBoolField(TEXT("diskPersistenceGuaranteed"),
                                bOriginalRestored && bOriginalFilePresent);
          AddAssetSaveReport(Failure, true, false, DestinationSaveState);
          if (bStageMovedAside) {
            UEditorAssetLibrary::DeleteAsset(StagedAssetPath);
          }
          Ctx.SendError(
              ErrorCodes::ERR_SAVE_FAILED,
              FString::Printf(
                  TEXT("Published object could not be saved; original restore=%s (%s)"),
                  bOriginalRestored ? TEXT("restored") : TEXT("failed"),
                  *OriginalRestoreError),
              Failure);
          return true;
        }

        TArray<UObject *> OldAssets = {BackupSequence};
        if (ObjectTools::ForceDeleteObjects(OldAssets, false) != 1) {
          WarningArray.Add(MakeShared<FJsonValueString>(FString::Printf(
              TEXT("Published overwrite but could not discard in-memory backup: %s"),
              *BackupAssetPath)));
        }
        UEditorAssetLibrary::DeleteAsset(StagedAssetPath);
      }

      DuplicatedAssets.Add(DestinationSequence->GetPathName());
    }
  }

  TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();

  TArray<TSharedPtr<FJsonValue>> DuplicatedArray;
  for (const FString &Path : DuplicatedAssets) {
    DuplicatedArray.Add(MakeShared<FJsonValueString>(Path));
  }
  if (DuplicatedArray.Num() > 0) {
    Resp->SetArrayField(TEXT("duplicatedAssets"), DuplicatedArray);
    if (DuplicatedAssets.Num() > 0) {
      UAnimSequence* FirstDuplicate =
          LoadObject<UAnimSequence>(nullptr, *DuplicatedAssets[0]);
      if (FirstDuplicate) {
        AddAssetVerification(Resp, FirstDuplicate);
      }
    }
  }

  if (SkippedAssets.Num() > 0) {
    TArray<TSharedPtr<FJsonValue>> SkippedArray;
    for (const FString &Path : SkippedAssets) {
      SkippedArray.Add(MakeShared<FJsonValueString>(Path));
    }
    Resp->SetArrayField(TEXT("skippedAssets"), SkippedArray);
  }

  if (WarningArray.Num() > 0) {
    Resp->SetArrayField(TEXT("warnings"), WarningArray);
  }

  Resp->SetStringField(TEXT("sourceSkeleton"), SourceSkeleton->GetPathName());
  Resp->SetStringField(TEXT("targetSkeleton"), TargetSkeleton->GetPathName());
  if (bSavePathProvided) {
    Resp->SetStringField(TEXT("savePath"), SavePath);
  }
  Resp->SetBoolField(TEXT("retargeted"), false);
  Resp->SetBoolField(TEXT("duplicatedWithSkeletonSwap"),
                     DuplicatedAssets.Num() > 0);
  const bool bHasDurableOutputs = DuplicatedAssets.Num() > 0;
  Resp->SetBoolField(TEXT("controlRigBindingCleared"), bHasDurableOutputs);
  Resp->SetBoolField(TEXT("diskPersistenceGuaranteed"), bHasDurableOutputs);
  AddAssetSaveReport(Resp, bHasDurableOutputs, bHasDurableOutputs);
  Ctx.SendSuccess(Resp);
  return true;
}

// ============================================================
// animation.play_montage - Play an animation montage on an actor
// (migrated from standalone HandlePlayAnimMontage)
// ============================================================
REGISTER_RPC_HANDLER("animation.play_montage", "animation",
    "Trigger AnimMontage playback on a placed actor's first SkeletalMeshComponent through its AnimInstance. Requires a SkeletalMeshComponent backed by an AnimBlueprint at runtime; in editor PIE the actor must be ticking.",
    RPC_PARAMS(
        RPC_PARAM_REQ("actorName", "string", "Display label or name of the actor that owns the SkeletalMeshComponent."),
        ParamAliasUtils::MakeAliasParamSpec(TEXT("montagePath"), TEXT("path"),
            TEXT("Asset path to an AnimMontage. The 'assetPath' alias is also accepted."),
            /*bRequired=*/true, TArray<FString>({TEXT("montagePath"), TEXT("assetPath")})),
        RPC_PARAM_OPT("playRate", "number", "Multiplier on Montage playback speed (1.0 = real-time, 2.0 = double-speed); defaults to 1.0.")
    ))
{
  FString ActorName = Ctx.GetString("actorName");
  if (ActorName.IsEmpty()) {
    Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, TEXT("actorName required"));
    return true;
  }

  FString MontagePath = Ctx.GetStringFirstOf({TEXT("montagePath"), TEXT("assetPath")});
  if (MontagePath.IsEmpty()) {
    Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, TEXT("montagePath required"));
    return true;
  }

  double PlayRate = Ctx.GetNumber("playRate", 1.0);

  if (!GEditor || !GEditor->GetEditorWorldContext().World()) {
    Ctx.SendError(ErrorCodes::ERR_EDITOR_NOT_AVAILABLE, TEXT("Editor world not available"));
    return true;
  }

  UEditorActorSubsystem *ActorSS =
      GEditor->GetEditorSubsystem<UEditorActorSubsystem>();
  if (!ActorSS) {
    Ctx.SendError(ErrorCodes::ERR_EDITOR_ACTOR_SUBSYSTEM_MISSING,
                  TEXT("EditorActorSubsystem not available"));
    return true;
  }

  TArray<AActor *> AllActors = ActorSS->GetAllLevelActors();
  AActor *TargetActor = nullptr;

  UWorld *World = GEditor->GetEditorWorldContext().World();
  for (TActorIterator<AActor> It(World); It; ++It) {
    AActor *Actor = *It;
    if (Actor) {
      if (Actor->GetActorLabel().Equals(ActorName, ESearchCase::IgnoreCase) ||
          Actor->GetName().Equals(ActorName, ESearchCase::IgnoreCase)) {
        TargetActor = Actor;
        break;
      }
    }
  }

  // Fallback to ActorSS search
  if (!TargetActor) {
    for (AActor *Actor : AllActors) {
      if (Actor &&
          (Actor->GetActorLabel().Equals(ActorName, ESearchCase::IgnoreCase) ||
           Actor->GetName().Equals(ActorName, ESearchCase::IgnoreCase))) {
        TargetActor = Actor;
        break;
      }
    }
  }

  if (!TargetActor) {
    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetStringField(TEXT("actorName"), ActorName);
    Resp->SetStringField(TEXT("montagePath"), MontagePath);
    Resp->SetNumberField(TEXT("playRate"), PlayRate);
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

  if (!ResolveAsset(MontagePath).bExists) {
    Ctx.SendError(ErrorCodes::ERR_ASSET_NOT_FOUND,
                  FString::Printf(TEXT("Montage asset not found: %s"), *MontagePath));
    return true;
  }

  UAnimMontage *Montage = LoadObject<UAnimMontage>(nullptr, *MontagePath);
  if (!Montage) {
    Ctx.SendError(ErrorCodes::ERR_ASSET_LOAD_FAILED,
                  FString::Printf(TEXT("Failed to load montage: %s"), *MontagePath));
    return true;
  }

  float MontageLength = 0.f;
  if (UAnimInstance *AnimInst = SkelMeshComp->GetAnimInstance()) {
    MontageLength =
        AnimInst->Montage_Play(Montage, static_cast<float>(PlayRate));
  } else {
    SkelMeshComp->SetAnimationMode(EAnimationMode::Type::AnimationSingleNode);
    SkelMeshComp->PlayAnimation(Montage, false);
  }

  TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
  Resp->SetStringField(TEXT("actorName"), ActorName);
  Resp->SetStringField(TEXT("montagePath"), MontagePath);
  Resp->SetNumberField(TEXT("playRate"), PlayRate);
  Resp->SetNumberField(TEXT("montageLength"), MontageLength);
  Resp->SetBoolField(TEXT("playing"), true);
  Ctx.SendSuccess(Resp);
  return true;
}

// ============================================================
// animation.add_notify - Add a notify event to an animation sequence
// ============================================================
REGISTER_RPC_HANDLER("animation.add_notify", "animation",
    "Add a single AnimNotify event to an AnimSequence at a given time. For more elaborate notify authoring (notify states, sync markers, montage notifies) prefer animation.authoring.add_notify / add_notify_state / add_montage_notify.",
    RPC_PARAMS(
        ParamAliasUtils::MakeAliasParamSpec(TEXT("animationPath"), TEXT("path"),
            TEXT("Asset path to the AnimSequence (also accepted as assetPath alias)."),
            /*bRequired=*/true, TArray<FString>({TEXT("animationPath"), TEXT("assetPath")})),
        RPC_PARAM_REQ("notifyName", "string", "Either an existing AnimNotify class name (creates a typed notify) or a free-form label (creates a generic notify with that name)."),
        RPC_PARAM_OPT("time", "number", "Position in seconds along the sequence where the notify fires; defaults to 0.")
    ))
{
  FString AssetPath = Ctx.GetStringFirstOf({TEXT("animationPath"), TEXT("assetPath")});

  FString NotifyName = Ctx.GetString("notifyName");
  double Time = Ctx.GetNumber("time", 0.0);

  if (AssetPath.IsEmpty() || NotifyName.IsEmpty()) {
    Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
                  TEXT("assetPath and notifyName are required for add_notify"));
    return true;
  }

  UAnimSequenceBase *AnimAsset = LoadObject<UAnimSequenceBase>(nullptr, *AssetPath);
  if (!AnimAsset) {
    Ctx.SendError(ErrorCodes::ERR_ASSET_NOT_FOUND,
                  FString::Printf(TEXT("Animation asset not found: %s"), *AssetPath));
    return true;
  }

  UAnimSequence *AnimSeq = Cast<UAnimSequence>(AnimAsset);
  if (!AnimSeq) {
    Ctx.SendError(ErrorCodes::ERR_INVALID_TYPE,
                  TEXT("Asset is not an AnimSequence (add_notify currently supports AnimSequence only)"));
    return true;
  }

  // Resolve Notify Class
  UClass *LoadedNotifyClass = nullptr;
  FString SearchName = NotifyName;

  LoadedNotifyClass = UClass::TryFindTypeSlow<UClass>(SearchName);
  if (!LoadedNotifyClass && !SearchName.StartsWith(TEXT("U"))) {
    LoadedNotifyClass = UClass::TryFindTypeSlow<UClass>(TEXT("U") + SearchName);
  }

  // Try standard Engine path variants
  if (!LoadedNotifyClass) {
    LoadedNotifyClass = FindObject<UClass>(
        nullptr,
        *FString::Printf(TEXT("/Script/Engine.%s"), *SearchName));
  }
  if (!LoadedNotifyClass && !SearchName.StartsWith(TEXT("U"))) {
    LoadedNotifyClass = FindObject<UClass>(
        nullptr,
        *FString::Printf(TEXT("/Script/Engine.U%s"), *SearchName));
  }

  AnimSeq->Modify();

  FAnimNotifyEvent NewEvent;
  NewEvent.Link(AnimSeq, (float)Time);
  NewEvent.TriggerTimeOffset = GetTriggerTimeOffsetForType(
      EAnimEventTriggerOffsets::OffsetBefore);

  if (LoadedNotifyClass) {
    UAnimNotify *NewNotify =
        NewObject<UAnimNotify>(AnimSeq, LoadedNotifyClass);
    NewEvent.Notify = NewNotify;
    NewEvent.NotifyName = FName(*NotifyName);
  } else {
    NewEvent.NotifyName = FName(*NotifyName);
  }

  AnimSeq->Notifies.Add(NewEvent);
  AnimSeq->PostEditChange();
  McpSafeAssetSave(AnimSeq);

  TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
  Resp->SetStringField(TEXT("assetPath"), AssetPath);
  Resp->SetStringField(TEXT("notifyName"), NotifyName);
  Resp->SetStringField(TEXT("notifyClass"),
                       LoadedNotifyClass ? LoadedNotifyClass->GetName() : TEXT("None"));
  Resp->SetNumberField(TEXT("time"), Time);
  Ctx.SendSuccess(Resp);
  return true;
}
