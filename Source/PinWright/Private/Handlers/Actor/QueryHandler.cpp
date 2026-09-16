// Copyright (c) 2026 Alexander Penkin. MIT License.

// QueryHandler.cpp - Migrated from PinWright_ControlHandlers.cpp
// Handles actor.list, actor.get, actor.find_by_name, actor.find_by_tag,
// actor.find_by_class

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ParamSpec.h"
#include "Handlers/ParamAliasUtils.h"
#include "Handlers/Actor/ActorNameParamUtils.h"
#include "Handlers/Actor/ActorQueryParamUtils.h"
#include "Handlers/Actor/ActorSubsystemUtils.h"
#include "PinWrightGlobals.h"
#include "PinWrightHelpers.h"
#include "PinWrightSubsystem.h"
#include "Utils/ActorUtils.h"
#include "Handlers/ErrorCodes.h"
#include "Utils/NameMatchFilter.h"
#include "Utils/StringUtils.h"
#include "Dom/JsonObject.h"

#include "Editor.h"
#include "EngineUtils.h"
#if __has_include("Subsystems/EditorActorSubsystem.h")
#include "Subsystems/EditorActorSubsystem.h"
#elif __has_include("EditorActorSubsystem.h")
#include "EditorActorSubsystem.h"
#endif
#include "GameFramework/Actor.h"

// Helper to create a JSON array from a vector
static TArray<TSharedPtr<FJsonValue>> MakeVecArray(const FVector& Vec)
{
    TArray<TSharedPtr<FJsonValue>> Arr;
    Arr.Add(MakeShared<FJsonValueNumber>(Vec.X));
    Arr.Add(MakeShared<FJsonValueNumber>(Vec.Y));
    Arr.Add(MakeShared<FJsonValueNumber>(Vec.Z));
    return Arr;
}

// ---- actor.list ----
REGISTER_RPC_HANDLER("actor.list", "actor", "Enumerate actors in the resolved world (PIE-first in 'auto') as label/name/path/class, plus the Outliner folder on request via fields=[\"folder\"]. Read-only. On any populated level the full list exceeds the inline display budget and spills to a file; narrow it inline with filter= (matched against name AND label, case-INSENSITIVE substring by default — pair with matchMode/caseSensitive for anchored or case-exact counting), limit= (max rows, totalMatches keeps reporting the untruncated total), or namesOnly=true / fields=[...] to drop the per-row path. For object-graph traversal of any UObject prefer system.inspect.list_objects.",
    RPC_PARAMS(
        RPC_PARAM_OPT("filter", "string", "Pattern matched against each actor's label AND internal name; an actor is kept if either matches. Default semantics are a case-INSENSITIVE SUBSTRING match, so filter:\"SH_\" also matches \"Brush_0\" (the lowercase sh_ inside it) — pass matchMode:'prefix' and/or caseSensitive:true to count by naming-convention prefix. Omit to return all actors."),
        NameMatch::MatchModeParam(TEXT("filter")),
        NameMatch::CaseSensitiveParam(TEXT("filter")),
        RPC_PARAM_DEF("world", "string", "Which world to query: 'editor', 'pie', or 'auto' (default).", "auto"),
        RPC_PARAM_DEF("limit", "number", "Max actors to return after filtering. 0 (default) = all. totalMatches always reports the full untruncated count so elision is detectable.", "0"),
        ParamAliasUtils::MakeAliasParamSpec(TEXT("fields"), TEXT("array|string"), TEXT("Case-insensitive allow-list of per-actor keys to return. Valid keys: label, name, path, class (the default four, returned when fields is omitted) and folder (the Outliner folder path, returned ONLY when named — it stays off the default row rather than widening every row of a verb that already spills). e.g. [\"label\",\"class\"] to drop the verbose path, or [\"name\",\"folder\"] to answer 'which folder is this actor in' without the full actor.describe tree. Any other key is REJECTED by name with INVALID_PARAMS naming the valid set; it is never silently dropped, because a projection made only of unrecognised keys would return rows that are empty objects. actor.describe carries the wider per-actor key set (level, guid, tags, transform, properties, components). A single string is also accepted, under this key or the singular 'field'."), /*bRequired=*/false, TArray<FString>{TEXT("fields"), TEXT("field")}),
        ParamAliasUtils::MakeAliasParamSpec(TEXT("namesOnly"), TEXT("bool"), TEXT("When true, returns only label+name+class per actor (drops the longest per-row field, path) — shorthand for the common 'just list what's here so I can pick' read. Snake_case names_only accepted. Ignored when fields is supplied."), /*bRequired=*/false, TArray<FString>{TEXT("namesOnly"), TEXT("names_only")})
    ))
{
  // filter + matchMode + caseSensitive resolve together: the default (substring,
  // case-insensitive) reproduces the pre-B-actor-list-filter-case-mismatch behaviour
  // exactly, and a malformed regex is rejected here rather than silently matching
  // nothing. See Utils/NameMatchFilter.h.
  NameMatch::FFilter Filter;
  if (!NameMatch::Require(Ctx, TArray<FString>{TEXT("filter")}, Filter)) {
    return true;
  }

  FString ResolvedMode;
  UWorld* World = McpActorUtils::ResolveQueryWorld(Ctx.GetString(TEXT("world")), ResolvedMode);

  // Per-actor field projection: an explicit fields allow-list (array or bare
  // string) wins; otherwise namesOnly drops the verbose path. Default keeps all
  // four keys so the unprojected output is byte-identical to the prior shape.
  // Shared with system.console.search via FHandlerContext::ReadFieldProjection;
  // the namesOnly column set (everything except the verbose path) is the argument.
  const TSet<FString> Fields = Ctx.ReadFieldProjection(
      {TEXT("label"), TEXT("name"), TEXT("class")});

  // Every key the row builder below can actually emit, lowercase to match the
  // lowercased set ReadFieldProjection returns. 'folder' is opt-in only: it is the
  // cheap answer to "where does this actor sit in the Outliner" (GetFolderPath), but
  // actor.list already spills on any populated level, so it is not added to the
  // default row.
  static const TArray<FString> EmittableFields = {
      TEXT("label"), TEXT("name"), TEXT("path"), TEXT("class"), TEXT("folder")};

  // A key this handler cannot emit is refused by name, never dropped. Dropping it
  // silently switched projection ON (any entry raises Fields.Num()) while matching no
  // column, so fields:["folder"] answered a matched actor with an empty {} row -
  // indistinguishable from "this actor has no such data", and fields:["name","folder"]
  // quietly returned one column of the two asked for. This is the courtesy the
  // dispatcher's UNKNOWN_PARAMS gate already extends to top-level keys; that gate only
  // ever sees the parameter names, never the values inside an array.
  TArray<FString> UnknownFields;
  for (const FString& Field : Fields) {
    if (!EmittableFields.Contains(Field)) {
      UnknownFields.Add(Field);
    }
  }
  if (UnknownFields.Num() > 0) {
    UnknownFields.Sort();
    Ctx.SendError(ErrorCodes::ERR_INVALID_PARAMS,
        FString::Printf(
            TEXT("Unknown fields[] entry(s) for 'actor.list': [%s]. Valid fields: [%s]. ")
            TEXT("Rejected rather than dropped: a projection made only of unrecognised ")
            TEXT("keys returns rows that are empty objects. For level, guid, tags, ")
            TEXT("transform, properties or components use actor.describe, which projects ")
            TEXT("the wider per-actor key set."),
            *FString::Join(UnknownFields, TEXT(", ")),
            *FString::Join(EmittableFields, TEXT(", "))));
    return true;
  }

  // The projection decision is fixed before the loop (the wanted-key set never
  // changes per actor), so resolve each key to a bool once instead of probing the
  // TSet — with a throwaway FString per call — for every emitted row. An empty set
  // means "no projection": want the default four. The probe keys are lowercase to match
  // the lowercased set ReadFieldProjection returns.
  const bool bProject = Fields.Num() > 0;
  const auto Wants = [&Fields, bProject](const TCHAR* Key) {
    return !bProject || Fields.Contains(FString(Key));
  };
  const bool bWantLabel = Wants(TEXT("label"));
  const bool bWantName = Wants(TEXT("name"));
  const bool bWantPath = Wants(TEXT("path"));
  const bool bWantClass = Wants(TEXT("class"));
  // Opt-in, so it is NOT read through Wants(): an absent projection means "the default
  // four", not "every emittable key".
  const bool bWantFolder = bProject && Fields.Contains(FString(TEXT("folder")));

  // limit truncates the returned array after filtering; 0 = all. totalMatches
  // below always reports the full filtered count regardless of the cap.
  const int32 Limit = Ctx.GetInt(TEXT("limit"), 0);

  TArray<TSharedPtr<FJsonValue>> ActorsArray;
  int32 TotalMatched = 0;

  if (World) {
    for (TActorIterator<AActor> It(World); It; ++It) {
      AActor *Actor = *It;
      if (!Actor)
        continue;
      // Materialize label/name only when something consumes them: the filter still
      // matches on both (an actor is kept if EITHER column matches), and the
      // projection may keep either column.
      const bool bFiltering = Filter.IsActive();
      const FString Label =
          (bFiltering || bWantLabel) ? Actor->GetActorLabel() : FString();
      const FString Name = (bFiltering || bWantName) ? Actor->GetName() : FString();
      if (bFiltering && !Filter.MatchesEither(Label, Name))
        continue;

      ++TotalMatched;
      if (Limit > 0 && ActorsArray.Num() >= Limit)
        continue; // keep counting TotalMatched, but stop appending rows

      TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
      if (bWantLabel)
        Entry->SetStringField(TEXT("label"), Label);
      if (bWantName)
        Entry->SetStringField(TEXT("name"), Name);
      if (bWantPath)
        Entry->SetStringField(TEXT("path"), Actor->GetPathName());
      if (bWantClass)
        Entry->SetStringField(TEXT("class"), Actor->GetClass()
                                                 ? Actor->GetClass()->GetPathName()
                                                 : TEXT(""));
      // Empty string for an actor sitting at the Outliner root - that is the folder it
      // is in, not a missing answer.
      if (bWantFolder)
        Entry->SetStringField(TEXT("folder"), Actor->GetFolderPath().ToString());
      ActorsArray.Add(MakeShared<FJsonValueObject>(Entry));
    }
  }

  TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
  Data->SetArrayField(TEXT("actors"), ActorsArray);
  // count = rows returned (unchanged semantics); totalMatches = full untruncated
  // match count so a caller can tell the list was capped by limit. totalMatches +
  // truncated is the shared filter+limit vocabulary of the sibling search handlers.
  Data->SetNumberField(TEXT("count"), ActorsArray.Num());
  Data->SetNumberField(TEXT("totalMatches"), TotalMatched);
  Data->SetBoolField(TEXT("truncated"), ActorsArray.Num() < TotalMatched);
  Data->SetStringField(TEXT("world"), ResolvedMode);
  Data->SetStringField(TEXT("worldPath"), World ? World->GetPathName() : FString());
  // Echoes filter + the resolved matchMode/caseSensitive (only when filtering, so an
  // unfiltered response keeps its exact prior shape). The mode echo is the signal the
  // original bug lacked: totalMatches looked authoritative with no way to tell which
  // matching semantics produced it.
  NameMatch::AddFilterEcho(Data, Filter, TEXT("filter"));
  Ctx.SendSuccess(Data);
  return true;
}

// ---- actor.get ----
REGISTER_RPC_HANDLER("actor.get", "actor", "Read summary metadata for one placed actor: name, label, path, class, tags, location, and scale. Use actor.get_components for the component list, actor.get_transform for full rotation.",
    RPC_PARAMS(
        ActorNameParamUtils::ActorNameParamReq(TEXT("string"), TEXT("Display label or name of the actor in the level. The objectPath and actorPath aliases (the key spawn/duplicate return) are also accepted." ACTORNAME_COLLISION_STEER))
    ))
{
  FString TargetName;
  if (!ActorNameParamUtils::RequireActorName(Ctx, TargetName)) {
    return true;
  }

  // Resolve with an explicit ambiguity verdict: a label matching several actors must not
  // be reported as "not found", and must never silently pick one.
  AActor *Found = nullptr;
  if (!ActorNameParamUtils::ResolveActorOrSendError(Ctx, nullptr, TargetName, Found)) {
    return true;
  }

  const FTransform Current = Found->GetActorTransform();
  TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
  Data->SetStringField(TEXT("name"), Found->GetName());
  Data->SetStringField(TEXT("label"), Found->GetActorLabel());
  Data->SetStringField(TEXT("path"), Found->GetPathName());
  Data->SetStringField(TEXT("class"), Found->GetClass()
                                          ? Found->GetClass()->GetPathName()
                                          : TEXT(""));

  TArray<TSharedPtr<FJsonValue>> TagsArray;
  for (const FName &Tag : Found->Tags) {
    TagsArray.Add(MakeShared<FJsonValueString>(Tag.ToString()));
  }
  Data->SetArrayField(TEXT("tags"), TagsArray);
  Data->SetArrayField(TEXT("location"), MakeVecArray(Current.GetLocation()));
  Data->SetArrayField(TEXT("scale"), MakeVecArray(Current.GetScale3D()));

  Ctx.SendSuccess(Data);
  return true;
}

// ---- actor.find_by_name ----
REGISTER_RPC_HANDLER("actor.find_by_name", "actor", "Search the current level for actors whose label, name, or path contains a case-insensitive substring. The fragment goes in the required 'name' param, which also accepts pattern, filter, query, substring and search for the same slot. Returns all matches with class info.",
    RPC_PARAMS(
        ActorQueryParamUtils::SearchFragmentParamReq(
            TEXT("Search substring matched case-insensitively against label/name/path. The canonical ")
            TEXT("param key is 'name'; because the VALUE is a substring, the spellings a caller reaches ")
            TEXT("for instead — pattern, filter, query, substring, search — are declared aliases for this ")
            TEXT("same slot rather than errors. Matching is always case-insensitive substring; ")
            TEXT("actor.list carries the matchMode/caseSensitive levers. Rejected if the fragment ")
            TEXT("contains '..', '/', or '\\' as a path-traversal guard."))
    ))
{
  // Read through ActorQueryParamUtils, not Ctx.GetString("name"): declaring the aliases on
  // the FParamSpec only gets the payload past the dispatcher's gates, and a body that still
  // read the canonical key alone would admit `pattern=` and then report "name required".
  FString Query = ActorQueryParamUtils::ResolveSearchFragment(Ctx);
  if (Query.IsEmpty()) {
    Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
        FString::Printf(TEXT("A non-empty search fragment is required in one of: %s"),
            *ActorQueryParamUtils::SearchFragmentKeyList()));
    return true;
  }

  // Security: Validate query format - reject path traversal attempts
  if (Query.Contains(TEXT("..")) || Query.Contains(TEXT("\\")) || Query.Contains(TEXT("/"))) {
    Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
        FString::Printf(TEXT("Invalid name query: '%s'. Path separators and traversal characters are not allowed."), *Query));
    return true;
  }

  // Both pointers are guarded through the shared seam. Unguarded, a null GEditor
  // faulted inside GetEditorSubsystem (it reads UEditorEngine::EditorSubsystemCollection)
  // and a null subsystem was dereferenced by the very next line - a crash on a read-only
  // query, not an error response. Now one registered EDITOR_ACTOR_SUBSYSTEM_MISSING error,
  // the same code the lighting / niagara / physics / VFX guards already emit.
  // The guard lives in Handlers/Actor/ActorSubsystemUtils.h because a live editor always
  // HAS the subsystem: the null branch is only reachable from a test that calls the seam
  // with nullptr (Tests/Actor/TestActorFindByNameSubsystemGuard.cpp).
  TArray<AActor *> AllActors;
  if (!ActorSubsystemUtils::RequireAllLevelActors(
          Ctx, ActorSubsystemUtils::GetEditorActorSubsystem(), AllActors)) {
    return true;
  }
  TArray<TSharedPtr<FJsonValue>> Matches;
  for (AActor *Actor : AllActors) {
    if (!Actor)
      continue;
    const FString Label = Actor->GetActorLabel();
    const FString Name = Actor->GetName();
    const FString Path = Actor->GetPathName();
    const bool bMatches = Label.Contains(Query, ESearchCase::IgnoreCase) ||
                          Name.Contains(Query, ESearchCase::IgnoreCase) ||
                          Path.Contains(Query, ESearchCase::IgnoreCase);
    if (bMatches) {
      TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
      Entry->SetStringField(TEXT("label"), Label);
      Entry->SetStringField(TEXT("name"), Name);
      Entry->SetStringField(TEXT("path"), Path);
      Entry->SetStringField(TEXT("class"),
                            Actor->GetClass() ? Actor->GetClass()->GetPathName()
                                              : TEXT(""));
      Matches.Add(MakeShared<FJsonValueObject>(Entry));
    }
  }

  TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
  Data->SetNumberField(TEXT("count"), Matches.Num());
  Data->SetArrayField(TEXT("actors"), Matches);
  Data->SetStringField(TEXT("query"), Query);
  Ctx.SendSuccess(Data);
  return true;
}

// ---- actor.find_by_tag ----
REGISTER_RPC_HANDLER("actor.find_by_tag", "actor", "List actors in the resolved world (PIE-first in 'auto') that match the given Tag; default is exact FName equality, but matchType='contains' broadens to substring on tag strings (case-insensitive). For read-only audit prefer system.inspect.find_by_tag.",
    RPC_PARAMS(
        RPC_PARAM_REQ("tag", "string", "Tag string; converted to FName for exact match. Path-traversal characters rejected."),
        RPC_PARAM_OPT("matchType", "string", "'exact' (default) — FName equality on each tag. 'contains' (alias 'substring') — case-insensitive substring on each tag string. Any other value is REJECTED with INVALID_MODE; it is never silently narrowed to 'exact'. The resolved value is echoed back as matchType."),
        RPC_PARAM_DEF("world", "string", "Which world to query: 'editor', 'pie', or 'auto' (default).", "auto")
    ))
{
  FString TagValue = Ctx.GetString(TEXT("tag"));
  if (TagValue.IsEmpty()) {
    Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, TEXT("tag required"));
    return true;
  }

  // Security: Validate tag format
  if (TagValue.Contains(TEXT("..")) || TagValue.Contains(TEXT("\\")) || TagValue.Contains(TEXT("/"))) {
    Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
        FString::Printf(TEXT("Invalid tag: '%s'. Path separators and traversal characters are not allowed."), *TagValue));
    return true;
  }

  // An unrecognised matchType used to fall through the `else` below into exact FName
  // equality, so `matchType:"substring"` or a typo returned a silently NARROWER set with
  // no error, no warning and no echo - a well-formed answer to a question the caller did
  // not ask. An accepted parameter is a promise; the only honest options are to honour it
  // or to refuse it, so an unknown value is refused by name here. NormalizeToken trims,
  // lowercases and folds '-' to '_', matching how NameMatch::Parse reads its own modes.
  const FString RawMatchType = Ctx.GetString(TEXT("matchType"));
  const FString MatchTypeToken = PinWright::NormalizeToken(RawMatchType);
  bool bContainsMode = false;
  if (!MatchTypeToken.IsEmpty())
  {
    if (MatchTypeToken == TEXT("contains") || MatchTypeToken == TEXT("substring"))
    {
      bContainsMode = true;
    }
    else if (MatchTypeToken != TEXT("exact"))
    {
      Ctx.SendError(ErrorCodes::ERR_INVALID_MODE,
          FString::Printf(
              TEXT("Unknown matchType '%s'. Valid values: exact (default, FName equality), ")
              TEXT("contains (alias substring, case-insensitive substring on each tag). ")
              TEXT("Rejected rather than silently matching 'exact', which would have returned ")
              TEXT("a narrower set that reads as 'nothing is tagged that way'."),
              *RawMatchType));
      return true;
    }
  }
  // Canonical spelling, never the alias the caller happened to send.
  const FString ResolvedMatchType = bContainsMode ? TEXT("contains") : TEXT("exact");
  FName TagName(*TagValue);

  FString ResolvedMode;
  UWorld* World = McpActorUtils::ResolveQueryWorld(Ctx.GetString(TEXT("world")), ResolvedMode);

  TArray<TSharedPtr<FJsonValue>> Matches;

  if (World) {
    for (TActorIterator<AActor> It(World); It; ++It) {
      AActor *Actor = *It;
      if (!Actor)
        continue;
      bool bMatches = false;
      if (bContainsMode) {
        for (const FName &Existing : Actor->Tags) {
          if (Existing.ToString().Contains(TagValue, ESearchCase::IgnoreCase)) {
            bMatches = true;
            break;
          }
        }
      } else {
        bMatches = Actor->ActorHasTag(TagName);
      }

      if (bMatches) {
        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        // Both identities: 'name' historically carried the LABEL here and keeps that
        // meaning for existing callers, but labels are not unique, so feeding one back
        // into a lookup can resolve a different actor. 'objectName' is GetName(), unique
        // within the level, and is the collision-safe key to look this row up by.
        Entry->SetStringField(TEXT("name"), Actor->GetActorLabel());
        Entry->SetStringField(TEXT("label"), Actor->GetActorLabel());
        Entry->SetStringField(TEXT("objectName"), Actor->GetName());
        Entry->SetStringField(TEXT("path"), Actor->GetPathName());
        Entry->SetStringField(TEXT("class"),
                              Actor->GetClass() ? Actor->GetClass()->GetPathName()
                                                : TEXT(""));
        Matches.Add(MakeShared<FJsonValueObject>(Entry));
      }
    }
  }

  TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
  Data->SetArrayField(TEXT("actors"), Matches);
  Data->SetNumberField(TEXT("count"), Matches.Num());
  Data->SetStringField(TEXT("world"), ResolvedMode);
  Data->SetStringField(TEXT("worldPath"), World ? World->GetPathName() : FString());
  // Echo the semantics that actually produced this row set. Without it a caller cannot
  // tell an exact-match answer from a substring one, which is what made the silent
  // fallback undetectable from the response.
  Data->SetStringField(TEXT("matchType"), ResolvedMatchType);
  Data->SetStringField(TEXT("tag"), TagValue);
  Ctx.SendSuccess(Data);
  return true;
}

// ---- actor.find_by_class ----
REGISTER_RPC_HANDLER("actor.find_by_class", "actor", "Iterate the resolved world (PIE-first in 'auto') for actors of the given UClass (and subclasses). Returns label and path. For read-only audit of any UObject prefer system.inspect.find_by_class.",
    RPC_PARAMS(
        ActorQueryParamUtils::FindClassNameParamReq(
            TEXT("Either a short class name (e.g. 'StaticMeshActor') or a full asset/script path ")
            TEXT("(e.g. '/Script/Engine.StaticMeshActor', '/Game/Foo/BP_Bar'). The shortened 'class' ")
            TEXT("spelling is a declared alias for this slot.")),
        RPC_PARAM_DEF("world", "string", "Which world to query: 'editor', 'pie', or 'auto' (default).", "auto")
    ))
{
  // 'class' was read here and advertised in the param help long before it was declared on
  // the spec, so the dispatcher rejected it before this line ever ran (the 644db343 defect).
  FString ClassName = ActorQueryParamUtils::ResolveFindClassName(Ctx);

  if (ClassName.IsEmpty()) {
    Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, TEXT("className or class is required"));
    return true;
  }

  // Security: Validate class name format
  if (ClassName.Contains(TEXT("..")) || ClassName.Contains(TEXT("\\"))) {
    Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
        FString::Printf(TEXT("Invalid class name format: '%s'. Path traversal characters are not allowed."), *ClassName));
    return true;
  }

  // Additional security: Reject absolute filesystem paths
  if (ClassName.StartsWith(TEXT("/")) && !IsValidMountPoint(ClassName)) {
    if (ClassName.Contains(TEXT("/etc/")) || ClassName.Contains(TEXT("/usr/")) ||
        ClassName.Contains(TEXT("/var/")) || ClassName.Contains(TEXT("/home/")) ||
        ClassName.Contains(TEXT("/root/")) || ClassName.Contains(TEXT("/tmp/")) ||
        ClassName.Contains(TEXT("C:\\")) || ClassName.Contains(TEXT("D:\\"))) {
      Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
          FString::Printf(TEXT("Invalid class name format: '%s'. Filesystem paths are not allowed."), *ClassName));
      return true;
    }
  }

  // Resolve the class through the shared ResolveUClass helper so short class
  // names ('StaticMeshActor', 'PointLight') resolve the same way they do for
  // the sibling class-resolving handlers (asset.list, system.inspect.inspect_class,
  // widget.create_widget_blueprint). The previous raw FindObject/LoadObject branch
  // only resolved full '/Script/...' paths and silently returned count:0 for short
  // names. A miss is now a CLASS_NOT_FOUND error so callers can distinguish
  // "class not found" from "zero actors of that class".
  UClass* ClassToFind = ResolveUClass(ClassName);
  if (!ClassToFind) {
    Ctx.SendError(ErrorCodes::ERR_CLASS_NOT_FOUND,
        FString::Printf(TEXT("Could not resolve class '%s'. Pass a short class name (e.g. 'StaticMeshActor') or a full asset/script path (e.g. '/Script/Engine.StaticMeshActor', '/Game/Foo/BP_Bar')."), *ClassName));
    return true;
  }

  FString ResolvedMode;
  UWorld* World = McpActorUtils::ResolveQueryWorld(Ctx.GetString(TEXT("world")), ResolvedMode);

  TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
  TArray<TSharedPtr<FJsonValue>> ActorsArray;

  if (World) {
    for (TActorIterator<AActor> It(World, ClassToFind); It; ++It) {
      if (AActor* Actor = *It) {
        TSharedPtr<FJsonObject> ActorObj = MakeShared<FJsonObject>();
        // Both identities: 'name' historically carried the LABEL here and keeps that
        // meaning for existing callers, but labels are not unique, so feeding one back
        // into a lookup can resolve a different actor. 'objectName' is GetName(), unique
        // within the level, and is the collision-safe key to look this row up by.
        ActorObj->SetStringField(TEXT("name"), Actor->GetActorLabel());
        ActorObj->SetStringField(TEXT("label"), Actor->GetActorLabel());
        ActorObj->SetStringField(TEXT("objectName"), Actor->GetName());
        ActorObj->SetStringField(TEXT("path"), Actor->GetPathName());
        ActorsArray.Add(MakeShared<FJsonValueObject>(ActorObj));
      }
    }
  }

  Data->SetArrayField(TEXT("actors"), ActorsArray);
  Data->SetNumberField(TEXT("count"), ActorsArray.Num());
  Data->SetStringField(TEXT("world"), ResolvedMode);
  Data->SetStringField(TEXT("worldPath"), World ? World->GetPathName() : FString());
  Ctx.SendSuccess(Data);
  return true;
}
