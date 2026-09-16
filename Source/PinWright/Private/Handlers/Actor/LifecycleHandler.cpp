// Copyright (c) 2026 Alexander Penkin. MIT License.

// LifecycleHandler.cpp - Migrated from PinWright_ControlHandlers.cpp
// Handles actor.delete, actor.duplicate, actor.delete_by_tag, actor.export

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ParamSpec.h"
#include "Handlers/ErrorCodes.h"
#include "Handlers/Actor/DynamicMeshCountProbe.h"
#include "PinWrightGlobals.h"
#include "PinWrightHelpers.h"
#include "PinWrightSubsystem.h"
#include "Utils/ActorUtils.h"
#include "Handlers/Actor/ActorNameParamUtils.h"
#include "Utils/JsonBuilders.h"
#include "Dom/JsonObject.h"

#include "Editor.h"
#if __has_include("Subsystems/EditorActorSubsystem.h")
#include "Subsystems/EditorActorSubsystem.h"
#elif __has_include("EditorActorSubsystem.h")
#include "EditorActorSubsystem.h"
#endif
#include "GameFramework/Actor.h"
#include "Engine/Level.h"
#include "Exporters/Exporter.h"
#include "LevelUtils.h"
#include "Misc/OutputDevice.h"

// ---- actor.delete ----
REGISTER_RPC_HANDLER("actor.delete", "actor", "Destroy one or more actors in the current level via UEditorActorSubsystem. Provide actorName for a single actor or actorNames for a batch (one of the two is required). Reports per-name deleted/missing arrays, plus an ambiguous[] array for names matching several actors (those are never deleted - display labels are not unique, so the verb refuses rather than guessing). Returns NOT_FOUND only when nothing was deleted and nothing was ambiguous.",
    RPC_PARAMS(
        RPC_PARAM_OPT("actorName", "string", "Display label or name of one actor to delete; ignored if actorNames is also provided."),
        RPC_PARAM_OPT("actorNames", "array", "Array of actor display labels/names; preferred for batch deletion. Empty/non-string entries are skipped.")
    ))
{
  const auto& Payload = Ctx.GetRawPayload();

  // Shared dual-accept gather (actorNames array, else singular actorName scalar),
  // also used by actor.select. actor.delete has no clear-on-empty semantics, so any
  // empty result — absent keys or a present-but-empty array — is simply rejected.
  TArray<FString> Targets;
  bool bArrayKeyPresent = false;
  if (!McpActorUtils::CollectActorNames(Payload, Targets, bArrayKeyPresent)) {
    Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("actorName or actorNames required"));
    return true;
  }

  UEditorActorSubsystem *ActorSS =
      GEditor->GetEditorSubsystem<UEditorActorSubsystem>();
  TArray<FString> Deleted;
  TArray<FString> Missing;
  TArray<TSharedPtr<FJsonValue>> Ambiguous;

  for (const FString &Name : Targets) {
    // Delete is destructive, so ambiguity must never resolve to "whichever matched first".
    // It is also not "missing": two actors DID answer to that name, and saying they did not
    // exist would be false. Bucket it separately with the candidates' unique internal names.
    const McpActorUtils::FActorResolution Resolution =
        McpActorUtils::ResolveActor(nullptr, Name);
    if (Resolution.IsAmbiguous()) {
      TSharedPtr<FJsonObject> AmbObj = MakeShared<FJsonObject>();
      AmbObj->SetStringField(TEXT("requestedName"), Name);
      TArray<TSharedPtr<FJsonValue>> Cands;
      for (AActor *Candidate : Resolution.Candidates) {
        if (!Candidate)
          continue;
        TSharedPtr<FJsonObject> C = MakeShared<FJsonObject>();
        C->SetStringField(TEXT("label"), Candidate->GetActorLabel());
        C->SetStringField(TEXT("name"), Candidate->GetName());
        C->SetStringField(TEXT("path"), Candidate->GetPathName());
        Cands.Add(MakeShared<FJsonValueObject>(C));
      }
      AmbObj->SetArrayField(TEXT("candidates"), Cands);
      Ambiguous.Add(MakeShared<FJsonValueObject>(AmbObj));
      continue;
    }

    AActor *Found = Resolution.Actor;
    if (!Found) {
      Missing.Add(Name);
      continue;
    }
    if (ActorSS->DestroyActor(Found)) {
      UE_LOG(LogPinWrightSubsystem, Display,
             TEXT("actor.delete: Deleted actor '%s'"), *Name);
      Deleted.Add(Name);
    } else
      Missing.Add(Name);
  }

  // An ambiguous name is an unmet request just as much as a missing one, so it must not be
  // able to report overall success.
  const bool bAllDeleted = Missing.Num() == 0 && Ambiguous.Num() == 0;
  const bool bAnyDeleted = Deleted.Num() > 0;
  TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
  Resp->SetBoolField(TEXT("success"), bAllDeleted);
  Resp->SetNumberField(TEXT("deletedCount"), Deleted.Num());

  TArray<TSharedPtr<FJsonValue>> DeletedArray;
  for (const FString &Name : Deleted)
    DeletedArray.Add(MakeShared<FJsonValueString>(Name));
  Resp->SetArrayField(TEXT("deleted"), DeletedArray);

  if (Missing.Num() > 0) {
    TArray<TSharedPtr<FJsonValue>> MissingArray;
    for (const FString &Name : Missing)
      MissingArray.Add(MakeShared<FJsonValueString>(Name));
    Resp->SetArrayField(TEXT("missing"), MissingArray);
  }
  if (Ambiguous.Num() > 0) {
    Resp->SetArrayField(TEXT("ambiguous"), Ambiguous);
    Resp->SetNumberField(TEXT("ambiguousCount"), Ambiguous.Num());
  }

  // Verification data
  Resp->SetBoolField(TEXT("existsAfter"), false);
  Resp->SetStringField(TEXT("action"), TEXT("control_actor:deleted"));

  if (!bAnyDeleted && Ambiguous.Num() > 0) {
    Ctx.SendError(ErrorCodes::ERR_AMBIGUOUS_ACTOR_NAME,
        FString::Printf(
            TEXT("Nothing was deleted: %d of the requested name(s) each match several actors. ")
            TEXT("Display labels are not unique in Unreal - re-issue with the unique internal ")
            TEXT("object names listed under ambiguous[].candidates[].name."),
            Ambiguous.Num()),
        Resp);
  } else if (!bAnyDeleted && Missing.Num() > 0) {
    Ctx.SendError(TEXT("NOT_FOUND"), TEXT("Actors not found"));
  } else {
    Ctx.SendSuccess(Resp);
  }
  return true;
}

// ---- actor.duplicate ----
REGISTER_RPC_HANDLER("actor.duplicate", "actor", "Duplicate a placed actor in the same world via UEditorActorSubsystem::DuplicateActor and translate the copy by an optional offset. The clone copies all properties and components but is unattached. Dynamic mesh actors are verified: the copy's triangle count must match the source, otherwise the duplicate is discarded and MESH_DUPLICATE_SUBSTITUTED is returned.",
    RPC_PARAMS(
        RPC_PARAM_REQ("actorName", "string", "Display label or name of the source actor."),
        RPC_PARAM_OPT("offset", "object", "World-space translation applied to the duplicate as {x,y,z} in unreal units; defaults to (0,0,0) which spawns at the source's location."),
        RPC_PARAM_OPT("newName", "string", "Custom label assigned to the duplicate after creation; UE auto-suffixes if omitted."),
        RPC_PARAM_DEF("allowPlaceholderMesh", "boolean", "Keep a dynamic mesh duplicate whose triangle count does not match the source (the engine's placeholder cube) and return success with meshPlaceholderSubstituted plus a warnings entry, instead of discarding it and failing MESH_DUPLICATE_SUBSTITUTED.", "false"),
        RPC_PARAM_DEF("allowSlowLargeMeshCopy", "boolean", "Raise geometry.DynamicMesh.TextBasedDupeTriThreshold above the source triangle count for the duration of this call so a large dynamic mesh copies via the engine's Base64 text path instead of being replaced by a placeholder. The engine calls that path 'quite slow' and it costs O(mesh) memory, so it is opt-in; the previous cvar value is restored before the call returns.", "false")
    ))
{
  FString TargetName = Ctx.GetString(TEXT("actorName"));
  if (TargetName.IsEmpty()) {
    Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("actorName required"));
    return true;
  }

  // Resolve with an explicit ambiguity verdict: a label matching several actors must not
  // be reported as "not found", and must never silently pick one.
  AActor *Found = nullptr;
  if (!ActorNameParamUtils::ResolveActorOrSendError(Ctx, nullptr, TargetName, Found)) {
    return true;
  }

  FVector Offset = Ctx.GetVector(TEXT("offset"), FVector::ZeroVector);
  UEditorActorSubsystem *ActorSS =
      GEditor->GetEditorSubsystem<UEditorActorSubsystem>();

  // A locked source level makes the engine's DuplicateActorsToLevel early-return
  // with zero new actors (EditorActor.cpp guards on FLevelUtils::IsLevelLocked
  // of the source level), surfacing only as a null result here. Detect that case
  // up front so the caller learns the cause and the recovery instead of a bare
  // DUPLICATE_FAILED; the generic error below remains the fallback for any other
  // unexplained null.
  if (ULevel *SourceLevel = Found->GetLevel()) {
    if (FLevelUtils::IsLevelLocked(SourceLevel)) {
      const FString LevelPackage = JsonBuilders::GetActorLevelPackageName(Found);
      Ctx.SendError(
          TEXT("LEVEL_LOCKED"),
          FString::Printf(
              TEXT("Cannot duplicate '%s': its level '%s' is locked. Unlock it "
                   "first via level.set_locked {levelPath:'%s', locked:false}."),
              *Found->GetActorLabel(), *LevelPackage, *LevelPackage));
      return true;
    }
  }

  // Dynamic mesh integrity, half one: snapshot the source's triangle count BEFORE the
  // duplicate. The engine's T3D paste silently swaps in a 12-triangle placeholder cube
  // and still reports success (UDynamicMesh.cpp:568 on 5.8), so a post-hoc count
  // comparison is the only caller-visible evidence that the copy is real. Non-dynamic-
  // mesh actors — the overwhelmingly common case — probe false and take the byte-
  // identical pre-existing path below.
  const bool bAllowPlaceholderMesh = Ctx.GetBool(TEXT("allowPlaceholderMesh"), false);
  const bool bAllowSlowLargeMeshCopy = Ctx.GetBool(TEXT("allowSlowLargeMeshCopy"), false);

  int64 SourceTriangles = 0;
  const bool bSourceProbed =
      DynamicMeshCountProbe::TryGetTotalTriangleCount(Found, SourceTriangles);

  AActor *Duplicated = nullptr;
  bool bSlowLargeMeshCopyApplied = false;
  {
    // Opt-in escape from the substitution instead of merely reporting it: widen the
    // engine's text-copy ceiling for exactly this call, restored by the guard's
    // destructor before the response is built.
    TUniquePtr<DynamicMeshCountProbe::FScopedDupeTriThresholdRaise> ThresholdGuard;
    if (bAllowSlowLargeMeshCopy && bSourceProbed) {
      ThresholdGuard =
          MakeUnique<DynamicMeshCountProbe::FScopedDupeTriThresholdRaise>(SourceTriangles);
      bSlowLargeMeshCopyApplied = ThresholdGuard->IsActive();
    }
    Duplicated = ActorSS->DuplicateActor(Found, Found->GetWorld(), Offset);
  }
  if (!Duplicated) {
    Ctx.SendError(TEXT("DUPLICATE_FAILED"), TEXT("Failed to duplicate actor"));
    return true;
  }

  // Half two: any inequality means the engine substituted geometry. Comparing counts
  // rather than predicting either cvar catches both triggers (over-threshold export and
  // copy-stash timeout) and stays correct if Epic changes the defaults.
  int64 DuplicateTriangles = 0;
  const bool bDuplicateProbed =
      DynamicMeshCountProbe::TryGetTotalTriangleCount(Duplicated, DuplicateTriangles);
  const DynamicMeshCountProbe::EDuplicateMeshVerdict MeshVerdict =
      DynamicMeshCountProbe::ClassifyDuplicate(bSourceProbed, bDuplicateProbed,
                                               SourceTriangles, DuplicateTriangles);

  if (MeshVerdict == DynamicMeshCountProbe::EDuplicateMeshVerdict::Substituted &&
      !bAllowPlaceholderMesh) {
    // Destroying restores the exact pre-call world state: this actor was created
    // microseconds ago by this call and has never been returned to anyone.
    ActorSS->DestroyActor(Duplicated);

    TSharedPtr<FJsonObject> ErrData = MakeShared<FJsonObject>();
    ErrData->SetNumberField(TEXT("sourceTriangles"), (double)SourceTriangles);
    ErrData->SetNumberField(TEXT("duplicateTriangles"), (double)DuplicateTriangles);
    // The duplicate no longer exists, so this echoes the SOURCE the caller named.
    ErrData->SetStringField(TEXT("actorName"), Found->GetActorLabel());
    ErrData->SetStringField(
        TEXT("remedy"),
        FString::Printf(
            TEXT("The engine replaced the duplicated dynamic mesh with a placeholder "
                 "cube (UDynamicMesh.cpp:568). Retry with allowSlowLargeMeshCopy:true "
                 "to raise geometry.DynamicMesh.TextBasedDupeTriThreshold above %lld "
                 "for the call (slow: Base64 text copy), bake the source to a static "
                 "mesh first, or pass allowPlaceholderMesh:true to accept the cube."),
            SourceTriangles));

    UE_LOG(LogPinWrightSubsystem, Warning,
           TEXT("actor.duplicate: discarded copy of '%s' - dynamic mesh triangle count "
                "%lld became %lld (engine placeholder substitution)"),
           *Found->GetActorLabel(), SourceTriangles, DuplicateTriangles);

    Ctx.SendError(ErrorCodes::ERR_MESH_DUPLICATE_SUBSTITUTED,
                  FString::Printf(TEXT("Duplicate of '%s' lost its dynamic mesh: the "
                                       "engine substituted a placeholder (%lld "
                                       "triangles, source had %lld)."),
                                  *Found->GetActorLabel(), DuplicateTriangles,
                                  SourceTriangles),
                  ErrData);
    return true;
  }

  FString NewName = Ctx.GetString(TEXT("newName"));
  if (!NewName.TrimStartAndEnd().IsEmpty())
    Duplicated->SetActorLabel(NewName);

  TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
  // 'source' keeps its meaning (the source actor's LABEL), but a label is not unique, so
  // echoing it back into a lookup can resolve a different actor. sourceObjectName is the
  // source's GetName() — unique within the level and the collision-safe key for the
  // caller to re-address the original by.
  Data->SetStringField(TEXT("source"), Found->GetActorLabel());
  Data->SetStringField(TEXT("sourceObjectName"), Found->GetName());
  Data->SetStringField(TEXT("actorName"), Duplicated->GetActorLabel());
  Data->SetStringField(TEXT("actorPath"), Duplicated->GetPathName());

  // Additive and only when both sides were actually probed: a NotProbed verdict leaves
  // the response byte-identical to the pre-integrity-check shape.
  if (MeshVerdict != DynamicMeshCountProbe::EDuplicateMeshVerdict::NotProbed) {
    Data->SetNumberField(TEXT("sourceTriangles"), (double)SourceTriangles);
    Data->SetNumberField(TEXT("duplicateTriangles"), (double)DuplicateTriangles);
  }
  if (bAllowSlowLargeMeshCopy) {
    // False here means the opt-in was inert (no cvar in this host, or the source could
    // not be probed), which is the difference between "tried and worked" and "ignored".
    Data->SetBoolField(TEXT("slowLargeMeshCopyApplied"), bSlowLargeMeshCopyApplied);
  }
  if (MeshVerdict == DynamicMeshCountProbe::EDuplicateMeshVerdict::Substituted) {
    Data->SetBoolField(TEXT("meshPlaceholderSubstituted"), true);
    TArray<TSharedPtr<FJsonValue>> WarnArray;
    WarnArray.Add(MakeShared<FJsonValueString>(FString::Printf(
        TEXT("Dynamic mesh was NOT copied: the engine substituted a placeholder "
             "(%lld triangles, source had %lld). Kept because "
             "allowPlaceholderMesh:true."),
        DuplicateTriangles, SourceTriangles)));
    Data->SetArrayField(TEXT("warnings"), WarnArray);
    UE_LOG(LogPinWrightSubsystem, Warning,
           TEXT("actor.duplicate: kept placeholder-substituted copy of '%s' "
                "(allowPlaceholderMesh) - %lld triangles became %lld"),
           *Found->GetActorLabel(), SourceTriangles, DuplicateTriangles);
  }

  AddActorVerification(Data, Duplicated);

  TArray<TSharedPtr<FJsonValue>> OffsetArray;
  OffsetArray.Add(MakeShared<FJsonValueNumber>(Offset.X));
  OffsetArray.Add(MakeShared<FJsonValueNumber>(Offset.Y));
  OffsetArray.Add(MakeShared<FJsonValueNumber>(Offset.Z));
  Data->SetArrayField(TEXT("offset"), OffsetArray);

  UE_LOG(LogPinWrightSubsystem, Display,
         TEXT("actor.duplicate: Duplicated '%s' to '%s'"), *Found->GetActorLabel(),
         *Duplicated->GetActorLabel());
  Ctx.SendSuccess(Data);
  return true;
}

// ---- actor.delete_by_tag ----
REGISTER_RPC_HANDLER("actor.delete_by_tag", "actor", "Bulk-destroy every actor in the current level whose Tags array contains the given FName. Returns deleted[] as the label list and the count, plus deletedActors[] as {label, objectName, path} rows (objectName is the unique internal name; labels are not unique); succeeds (count=0) when no actors match.",
    RPC_PARAMS(
        RPC_PARAM_REQ("tag", "string", "Tag string converted to FName for matching (case-sensitive); compares against AActor::ActorHasTag.")
    ))
{
  FString TagValue = Ctx.GetString(TEXT("tag"));
  if (TagValue.IsEmpty()) {
    Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("tag required"));
    return true;
  }

  const FName TagName(*TagValue);
  UEditorActorSubsystem *ActorSS =
      GEditor->GetEditorSubsystem<UEditorActorSubsystem>();
  const TArray<AActor *> AllActors = ActorSS->GetAllLevelActors();

  // All three identity strings are read BEFORE DestroyActor: once the actor is destroyed
  // neither GetName() nor GetPathName() can be recovered, and the label alone is not a
  // usable receipt because labels are not unique.
  struct FDeletedIdentity {
    FString Label;
    FString ObjectName;
    FString Path;
  };
  TArray<FDeletedIdentity> Deleted;

  for (AActor *Actor : AllActors) {
    if (!Actor)
      continue;
    if (Actor->ActorHasTag(TagName)) {
      const FDeletedIdentity Identity{Actor->GetActorLabel(), Actor->GetName(),
                                      Actor->GetPathName()};
      if (ActorSS->DestroyActor(Actor))
        Deleted.Add(Identity);
    }
  }

  TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
  Data->SetStringField(TEXT("tag"), TagName.ToString());
  Data->SetNumberField(TEXT("deletedCount"), Deleted.Num());
  // 'deleted' keeps its original shape - an array of bare LABEL strings - so existing
  // callers are unaffected. 'deletedActors' is the additive richer form: label is what the
  // outliner showed and is NOT unique, objectName is the collision-safe GetName(), and both
  // are captured before DestroyActor because neither is recoverable afterwards.
  TArray<TSharedPtr<FJsonValue>> DeletedArray;
  TArray<TSharedPtr<FJsonValue>> DeletedActorsArray;
  for (const FDeletedIdentity &Identity : Deleted) {
    DeletedArray.Add(MakeShared<FJsonValueString>(Identity.Label));

    TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
    Entry->SetStringField(TEXT("label"), Identity.Label);
    Entry->SetStringField(TEXT("objectName"), Identity.ObjectName);
    Entry->SetStringField(TEXT("path"), Identity.Path);
    DeletedActorsArray.Add(MakeShared<FJsonValueObject>(Entry));
  }
  Data->SetArrayField(TEXT("deleted"), DeletedArray);
  Data->SetArrayField(TEXT("deletedActors"), DeletedActorsArray);

  Data->SetBoolField(TEXT("existsAfter"), false);
  Data->SetStringField(TEXT("action"), TEXT("control_actor:deleted"));

  Ctx.SendSuccess(Data);
  return true;
}

// ---- actor.export ----
REGISTER_RPC_HANDLER("actor.export", "actor", "Serialize an actor and its components to UE's T3D (text) format and return the text in the response. Useful for capturing actor state for later re-import or for diffing scene changes.",
    RPC_PARAMS(
        RPC_PARAM_REQ("actorName", "string", "Display label or name of the actor to export.")
    ))
{
  FString TargetName = Ctx.GetString(TEXT("actorName"));
  if (TargetName.IsEmpty()) {
    Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("actorName required"));
    return true;
  }

  // Resolve with an explicit ambiguity verdict: a label matching several actors must not
  // be reported as "not found", and must never silently pick one.
  AActor *Found = nullptr;
  if (!ActorNameParamUtils::ResolveActorOrSendError(Ctx, nullptr, TargetName, Found)) {
    return true;
  }

  FMcpOutputCapture OutputCapture;
  UExporter::ExportToOutputDevice(nullptr, Found, nullptr, OutputCapture,
                                  TEXT("T3D"), 0, 0, false);
  FString OutputString = FString::Join(OutputCapture.Consume(), TEXT("\n"));

  TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
  Data->SetStringField(TEXT("t3d"), OutputString);
  Data->SetStringField(TEXT("actorName"), Found->GetActorLabel());
  Ctx.SendSuccess(Data);
  return true;
}
