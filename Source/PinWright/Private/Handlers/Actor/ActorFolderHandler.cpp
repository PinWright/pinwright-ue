// Copyright (c) 2026 Alexander Penkin. MIT License.

// ActorFolderHandler.cpp - actor.set_folder
// Assigns one or more placed actors to a World Outliner folder path via
// AActor::SetFolderPath, reporting per-actor updated / missing lists.

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ParamSpec.h"
#include "Handlers/ParamAliasUtils.h"
#include "PinWrightGlobals.h"
#include "PinWrightSubsystem.h"
#include "Utils/ActorUtils.h"
#include "Handlers/ErrorCodes.h"
#include "Dom/JsonObject.h"

#include "GameFramework/Actor.h"

namespace
{
  // Single source of truth used by BOTH the FParamSpec alias registration and the
  // body-side GetStringFirstOf read, so the schema and the handler can never drift.
  // Uniquely named to avoid an ODR clash with sibling handlers under a Unity merge.
  const TArray<FString>& ActorSetFolderPathKeys()
  {
    static const TArray<FString> Keys = { TEXT("folderPath"), TEXT("folder") };
    return Keys;
  }
}

// ---- actor.set_folder ----
REGISTER_RPC_HANDLER("actor.set_folder", "actor", "Assign one or more actors to a World Outliner folder path via SetFolderPath. Accepts a single actorName or an actorNames array; reports per-actor updated / missing lists. Folders are an editor-only organizational tree (e.g. 'Prototype/Walls'), created implicitly on assignment.",
    RPC_PARAMS(
        RPC_PARAM_OPT("actorName", "string", "Display label or name of one actor to move; ignored if actorNames is also provided."),
        RPC_PARAM_OPT("actorNames", "array", "Array of actor display labels/names to move as a batch."),
        // `folder` is a schema-registered alias, not merely the body-side
        // GetStringFirstOf resolution below, so a folder-only payload passes the
        // dispatcher's ValidateHandlerParams required-param check instead of
        // hard-failing MISSING_REQUIRED_PARAM before the handler body ever runs.
        // The sibling actor.spawn_batch spells this slot `folder`, and this verb's
        // own response emits `folder` per row, so the key a caller reads back has
        // to be the key it can feed to the next call.
        ParamAliasUtils::MakeAliasParamSpec(TEXT("folderPath"), TEXT("string"),
            TEXT("World Outliner folder path to assign, e.g. 'Prototype/Walls'. Nested folders use '/' separators. The 'folder' alias - the spelling actor.spawn_batch uses and the one this verb echoes back - is also accepted."),
            /*bRequired=*/true, ActorSetFolderPathKeys())
    ))
{
  const auto& Payload = Ctx.GetRawPayload();

  // Dual-accept the actor-identity slot (actorNames array, else singular actorName).
  TArray<FString> Targets;
  bool bArrayKeyPresent = false;
  if (!McpActorUtils::CollectActorNames(Payload, Targets, bArrayKeyPresent))
  {
    Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("actorName or actorNames required"));
    return true;
  }

  // Dual-accept the folder slot (canonical folderPath, else the `folder` alias).
  const FString FolderPath = Ctx.GetStringFirstOf(ActorSetFolderPathKeys());
  if (FolderPath.IsEmpty())
  {
    Ctx.SendError(TEXT("MISSING_PARAM"), TEXT("folderPath (or folder) is required"));
    return true;
  }
  const FName FolderName(*FolderPath);

  TArray<TSharedPtr<FJsonValue>> Updated;
  TArray<FString> Missing;
  TArray<TSharedPtr<FJsonValue>> Ambiguous;
  for (const FString& Name : Targets)
  {
    // Resolve with an explicit ambiguity verdict. A name matching several actors is NOT
    // "missing" - reporting it that way told the caller the actor did not exist when in
    // fact two did, and the old first-match-wins moved exactly one of them under a
    // success. Ambiguous names are bucketed separately, with candidates, and moved.
    const McpActorUtils::FActorResolution Resolution = McpActorUtils::ResolveActor(nullptr, Name);
    if (Resolution.IsAmbiguous())
    {
      TSharedPtr<FJsonObject> AmbObj = MakeShared<FJsonObject>();
      AmbObj->SetStringField(TEXT("requestedName"), Name);
      TArray<TSharedPtr<FJsonValue>> Cands;
      for (AActor* Candidate : Resolution.Candidates)
      {
        if (!Candidate) continue;
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

    AActor* Found = Resolution.Actor;
    if (!Found)
    {
      Missing.Add(Name);
      continue;
    }

    Found->Modify();
    Found->SetFolderPath(FolderName);

    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
    // Both identities: `name` stays the display label for backward compatibility, and
    // `objectName` carries the unique internal FName so the row can be fed back into a
    // lookup that resolves deterministically.
    Obj->SetStringField(TEXT("name"), Found->GetActorLabel());
    Obj->SetStringField(TEXT("label"), Found->GetActorLabel());
    Obj->SetStringField(TEXT("objectName"), Found->GetName());
    Obj->SetStringField(TEXT("path"), Found->GetPathName());
    Obj->SetStringField(TEXT("folder"), Found->GetFolderPath().ToString());
    Updated.Add(MakeShared<FJsonValueObject>(Obj));
  }

  TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
  // Echo both spellings at top level: `folderPath` for backward compatibility and
  // `folder` so the response agrees with the per-row key and with the accepted
  // request key, making the result feedable straight back into the next call.
  Data->SetStringField(TEXT("folderPath"), FolderPath);
  Data->SetStringField(TEXT("folder"), FolderPath);
  Data->SetArrayField(TEXT("updated"), Updated);
  Data->SetNumberField(TEXT("updatedCount"), Updated.Num());
  if (Missing.Num() > 0)
  {
    TArray<TSharedPtr<FJsonValue>> MissingArr;
    for (const FString& Name : Missing)
    {
      MissingArr.Add(MakeShared<FJsonValueString>(Name));
    }
    Data->SetArrayField(TEXT("missing"), MissingArr);
  }
  if (Ambiguous.Num() > 0)
  {
    Data->SetArrayField(TEXT("ambiguous"), Ambiguous);
    Data->SetNumberField(TEXT("ambiguousCount"), Ambiguous.Num());
  }

  if (Updated.Num() == 0)
  {
    // Distinguish "nothing by that name" from "the name was not specific enough". The
    // recovery differs: one is a typo, the other needs a unique internal object name.
    if (Ambiguous.Num() > 0)
    {
      Ctx.SendError(ErrorCodes::ERR_AMBIGUOUS_ACTOR_NAME,
          FString::Printf(TEXT("No actor was moved: %d of the requested name(s) each match several actors. ")
                          TEXT("Display labels are not unique in Unreal - re-issue with the unique internal ")
                          TEXT("object names listed under ambiguous[].candidates[].name."), Ambiguous.Num()),
          Data);
      return true;
    }
    Ctx.SendError(TEXT("ACTOR_NOT_FOUND"), TEXT("No matching actors found to move"));
    return true;
  }

  UE_LOG(LogPinWrightSubsystem, Display,
         TEXT("actor.set_folder: Moved %d actor(s) to '%s'"), Updated.Num(), *FolderPath);
  Ctx.SendSuccess(Data);
  return true;
}
