// Copyright (c) 2026 Alexander Penkin. MIT License.

// ActorSubsystemUtils.h - the guarded seam between an actor.* handler and
// UEditorActorSubsystem::GetAllLevelActors().
//
// Motivating defect: actor.find_by_name (Handlers/Actor/QueryHandler.cpp) read
//
//     UEditorActorSubsystem *ActorSS = GEditor->GetEditorSubsystem<UEditorActorSubsystem>();
//     const TArray<AActor *> AllActors = ActorSS->GetAllLevelActors();
//
// with no check on either pointer. Both halves are real faults, and they fail
// differently, which is why the guard covers both:
//
//   - GEditor == nullptr is a hard crash on the FIRST line, before ActorSS is
//     even assigned: UEditorEngine::GetEditorSubsystem<T>() reads the engine's
//     EditorSubsystemCollection member (EditorEngine.h:3383-3389 on UE 5.8), so
//     it is a member-offset load through a null this.
//   - ActorSS == nullptr is a member call through a null pointer on the SECOND
//     line. UEditorActorSubsystem::GetAllLevelActors touches no member of this
//     (EditorActorSubsystem.cpp:369-396 on UE 5.8), so it is undefined behaviour
//     that on today's MSVC codegen can fall through and return a populated array
//     from a subsystem the caller never held - a silent wrong answer rather than
//     an honest failure. Neither outcome is a contract; both become one
//     EDITOR_ACTOR_SUBSYSTEM_MISSING error response here.
//
// Shape follows Handlers/Environment/PostProcessVolumeUtils.h - header-only /
// inline, so any handler TU picks it up without growing the link surface, and it
// sends the same EDITOR_ACTOR_SUBSYSTEM_MISSING code the LightingHandler /
// NiagaraHandler / EffectHandler / PhysicsHandler guards already emit.
//
// The split into QueryAllLevelActors (pure) + RequireAllLevelActors (sends
// through FHandlerContext) exists for testability: a live editor always HAS the
// subsystem, so the null branch is unreachable from a handler-level test. The
// pure form takes the subsystem pointer as an argument, so a test can pass
// nullptr and pin the branch directly - see
// Tests/Actor/TestActorFindByNameSubsystemGuard.cpp.
#pragma once

#include "CoreMinimal.h"
#include "Editor.h"
#include "GameFramework/Actor.h"
#include "Handlers/ErrorCodes.h"
#include "Handlers/HandlerContext.h"
#if __has_include("Subsystems/EditorActorSubsystem.h")
#include "Subsystems/EditorActorSubsystem.h"
#elif __has_include("EditorActorSubsystem.h")
#include "EditorActorSubsystem.h"
#endif

namespace ActorSubsystemUtils
{

// Single spelling of the failure message, so the handler response, the seam's
// pure result and the test assertion cannot drift. Opens with the phrase the
// sibling guards already emit ("EditorActorSubsystem not available") so the
// existing greps over that vocabulary keep matching, then says what the caller
// can do about it.
inline const TCHAR* EditorActorSubsystemMissingMessage()
{
    return TEXT("EditorActorSubsystem not available: GEditor exposes no UEditorActorSubsystem, "
                "so the level actor list cannot be read. This is the editor not being fully "
                "initialized (or running as a commandlet); retry once the editor has finished "
                "loading.");
}

// Result of asking for the level actor list. bOk false means Actors is empty and
// ErrorCode/ErrorMessage carry a registered failure - never a half-filled array.
struct FLevelActorsQuery
{
    bool bOk = false;
    FString ErrorCode;
    FString ErrorMessage;
    TArray<AActor*> Actors;
};

// Pure form: no globals, no FHandlerContext, no editor required. Passing nullptr
// is the whole point - it is the branch the crash used to take.
inline FLevelActorsQuery QueryAllLevelActors(UEditorActorSubsystem* ActorSS)
{
    FLevelActorsQuery Query;
    if (!ActorSS)
    {
        Query.ErrorCode = ErrorCodes::ERR_EDITOR_ACTOR_SUBSYSTEM_MISSING;
        Query.ErrorMessage = EditorActorSubsystemMissingMessage();
        return Query;
    }
    Query.bOk = true;
    Query.Actors = ActorSS->GetAllLevelActors();
    return Query;
}

// GEditor-safe acquisition. Kept separate from the guard so the guard stays pure
// and the GEditor read has exactly one null check on the surface.
inline UEditorActorSubsystem* GetEditorActorSubsystem()
{
    return GEditor ? GEditor->GetEditorSubsystem<UEditorActorSubsystem>() : nullptr;
}

// Handler-facing form. Returns false having ALREADY dispatched the error
// response, so callers early-return true (request handled) exactly like the
// FindOrSpawnUnboundPPV nullptr contract. OutActors is reset on failure so a
// caller that ignores the bool still iterates nothing rather than stale rows.
inline bool RequireAllLevelActors(FHandlerContext& Ctx, UEditorActorSubsystem* ActorSS,
                                  TArray<AActor*>& OutActors)
{
    FLevelActorsQuery Query = QueryAllLevelActors(ActorSS);
    if (!Query.bOk)
    {
        OutActors.Reset();
        Ctx.SendError(Query.ErrorCode, Query.ErrorMessage);
        return false;
    }
    OutActors = MoveTemp(Query.Actors);
    return true;
}

} // namespace ActorSubsystemUtils
