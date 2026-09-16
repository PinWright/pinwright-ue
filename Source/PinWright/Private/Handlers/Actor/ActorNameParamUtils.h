// Copyright (c) 2026 Alexander Penkin. MIT License.

// Shared param-alias helper for the actor.* read/consumer verbs whose target slot
// is the actor-identity key.
//
// The slot is canonically `actorName` (a display label or internal object name), and
// the read body resolves the value via McpActorUtils::FindActorByName, which already
// matches a label, an internal name, OR a full object path. But its producers spell the
// returned reference differently: actor.spawn / actor.spawn_instance / actor.duplicate
// hand the spawned actor back under `actorPath` (SpawnHandler.cpp / LifecycleHandler.cpp),
// and `actorPath` is a declared INPUT slot in volume.* / world.* / sequencer.* / gas.*.
// A caller who reuses `actorPath` (or the path-shaped `objectPath`) against an actor.*
// reader hard-failed MISSING_REQUIRED_PARAM 'actorName' at the wire level, because the
// spec declared only the canonical name with no aliases.
//
// The dispatcher honors FParamSpec aliases both for required-param satisfaction
// (PayloadHasParamOrAlias) and the known-params set (AddKnownParamNames), so annotating
// the spec via ParamAliasUtils makes `objectPath` / `actorPath` resolve at validation; the
// body must read the value via GetStringFirstOf(ActorNameKeys()) for the alias to reach
// the handler. Mirrors AssetPathParamUtils for the asset-path slot.
#pragma once

#include "CoreMinimal.h"
#include "Handlers/ParamSpec.h"
#include "Handlers/ParamAliasUtils.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ErrorCodes.h"
#include "Utils/ActorUtils.h"
#include "GameFramework/Actor.h"
#include "Dom/JsonObject.h"

namespace ActorNameParamUtils
{

// Candidate wire names for the actor-identity slot, canonical first. Used both to
// populate FParamSpec aliases at registration and to read the value body-side via
// FHandlerContext::GetStringFirstOf so the alias resolves end-to-end. `objectPath` and
// `actorPath` are both path-shaped (FindActorByName matches by GetPathName too);
// `actor_name` is the snake_case variant of the canonical key.
inline const TArray<FString>& ActorNameKeys()
{
    static const TArray<FString> Keys = {
        TEXT("actorName"),
        TEXT("objectPath"),
        TEXT("actorPath"),
        TEXT("actor_name")
    };
    return Keys;
}

// Construct the required actor-identity FParamSpec with the alias set above attached.
// The canonical name is the head of ActorNameKeys() so it is defined in exactly one place.
inline FParamSpec ActorNameParamReq(const TCHAR* Type, const TCHAR* Desc)
{
    return ParamAliasUtils::MakeAliasParamSpec(*ActorNameKeys()[0], Type, Desc,
        /*bRequired=*/true, ActorNameKeys());
}

// Body-side counterpart to the alias-annotated spec: resolves the actor-identity value
// from the first matching key (canonical first). Returns the empty string when none of
// the keys is present; the caller then sends its own domain error.
inline FString ResolveActorName(const FHandlerContext& Ctx)
{
    return Ctx.GetStringFirstOf(ActorNameKeys());
}

// Resolve-and-require counterpart (mirrors AssetPathParamUtils::RequireAssetPathRaw):
// reads the actor-identity value from the first matching key and, when none is present,
// sends one canonical error naming every accepted slot so the message stays in lockstep
// with ActorNameKeys(). Returns false (after sending the error) when empty; the body
// then does `if (!RequireActorName(Ctx, Out)) return true;`.
inline bool RequireActorName(const FHandlerContext& Ctx, FString& Out)
{
    Out = Ctx.GetStringFirstOf(ActorNameKeys());
    if (Out.IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"),
            FString::Printf(TEXT("One of these is required: %s"),
                *FString::Join(ActorNameKeys(), TEXT(", "))));
        return false;
    }
    return true;
}

// Emit the canonical AMBIGUOUS_ACTOR_NAME error for a resolution that matched several
// actors. The payload lists every non-null candidate as {label, name, path, class} so the
// caller can re-issue the same verb against a unique key without a second round trip.
// Picking a candidate here would be exactly the defect this error exists to prevent
// (rpc-design.md §1).
inline void SendAmbiguousActorError(
    const FHandlerContext& Ctx,
    const FString& Identifier,
    const McpActorUtils::FActorResolution& Resolution)
{
    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("requestedName"), Identifier);
    const TCHAR* MatchedBy = TEXT("labelSubstring");
    switch (Resolution.MatchedBy)
    {
    case McpActorUtils::EActorMatchKind::ObjectPath: MatchedBy = TEXT("objectPath"); break;
    case McpActorUtils::EActorMatchKind::ObjectName: MatchedBy = TEXT("objectName"); break;
    case McpActorUtils::EActorMatchKind::Label: MatchedBy = TEXT("label"); break;
    default: break;
    }
    Data->SetStringField(TEXT("matchedBy"), MatchedBy);

    TArray<TSharedPtr<FJsonValue>> Candidates;
    int32 EmittedCandidateCount = 0;
    for (AActor* Candidate : Resolution.Candidates)
    {
        if (!Candidate)
        {
            continue;
        }
        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("label"), Candidate->GetActorLabel());
        Entry->SetStringField(TEXT("name"), Candidate->GetName());
        Entry->SetStringField(TEXT("path"), Candidate->GetPathName());
        Entry->SetStringField(TEXT("class"), Candidate->GetClass()->GetPathName());
        Candidates.Add(MakeShared<FJsonValueObject>(Entry));
        ++EmittedCandidateCount;
    }
    Data->SetNumberField(TEXT("candidateCount"), EmittedCandidateCount);
    Data->SetArrayField(TEXT("candidates"), Candidates);

    const TCHAR* Recovery = TEXT("Re-issue with one of the full object paths");
    if (Resolution.MatchedBy == McpActorUtils::EActorMatchKind::Label)
    {
        Recovery = TEXT("Re-issue with one of the unique internal object names or full object paths");
    }

    Ctx.SendError(ErrorCodes::ERR_AMBIGUOUS_ACTOR_NAME,
        FString::Printf(
            TEXT("'%s' matches %d actors by %s, so it does not identify one. %s; each candidate's ")
            TEXT("label, name, path and class is in the candidates array."),
            *Identifier, EmittedCandidateCount, MatchedBy, Recovery),
        Data);
}

// Resolve an already-read identifier, sending the RIGHT error when it does not name exactly
// one actor: AMBIGUOUS_ACTOR_NAME (with candidates) when several match, ACTOR_NOT_FOUND when
// none does. Use this in place of a bare
//   AActor* Found = McpActorUtils::FindActorByName(nullptr, TargetName);
//   if (!Found) { Ctx.SendError(TEXT("ACTOR_NOT_FOUND"), TEXT("Actor not found")); return true; }
// because FindActorByName now returns nullptr for BOTH cases, which would make a verb report
// "actor not found" about a name that in fact matched two actors - a false statement, and the
// §1 defect class in miniature. Bodies do:
//   AActor* Found = nullptr;
//   if (!ActorNameParamUtils::ResolveActorOrSendError(Ctx, nullptr, TargetName, Found)) return true;
inline bool ResolveActorOrSendError(
    const FHandlerContext& Ctx,
    UWorld* World,
    const FString& Identifier,
    AActor*& OutActor)
{
    OutActor = nullptr;
    const McpActorUtils::FActorResolution Resolution = McpActorUtils::ResolveActor(World, Identifier);
    if (Resolution.IsAmbiguous())
    {
        SendAmbiguousActorError(Ctx, Identifier, Resolution);
        return false;
    }
    if (!Resolution.IsResolved())
    {
        Ctx.SendError(ErrorCodes::ERR_ACTOR_NOT_FOUND,
            FString::Printf(TEXT("No actor matches '%s' by display label, internal object name, or object path."),
                *Identifier));
        return false;
    }
    OutActor = Resolution.Actor;
    return true;
}

// Read the actor-identity value from the first matching key and resolve it in one step,
// sending the MISSING-slot error, AMBIGUOUS_ACTOR_NAME, or ACTOR_NOT_FOUND as appropriate.
// OutName receives the identifier as supplied, for echoing back.
inline bool RequireResolvedActor(
    const FHandlerContext& Ctx,
    UWorld* World,
    AActor*& OutActor,
    FString* OutName = nullptr)
{
    OutActor = nullptr;
    FString Identifier;
    if (!RequireActorName(Ctx, Identifier))
    {
        return false;
    }
    if (OutName)
    {
        *OutName = Identifier;
    }
    return ResolveActorOrSendError(Ctx, World, Identifier, OutActor);
}

} // namespace ActorNameParamUtils
