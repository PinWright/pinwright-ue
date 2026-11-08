// Copyright (c) 2026 Alexander Penkin. MIT License.

// Live-actor networking verbs: the game-safe half of the editor module's NetworkingHandler.cpp
// (which keeps the Blueprint replication authoring verbs and networking.get_networking_info).

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ParamSpec.h"
#include "Dispatch/PinWrightWorld.h"
#include "PinWrightHelpers.h"
#include "Utils/ActorUtils.h"
#include "Utils/AssetResolveUtils.h"
#include "Dom/JsonObject.h"

#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"

namespace NetworkingHelpersNew
{
    // The editor world in the editor (never PIE, as before the split), the game world in a game
    // process. FindActorByNameSimple honours the passed world; see the editor file for why the
    // richer PIE-first FindActorByName is not used.
    static UWorld* ResolveNetworkingWorld()
    {
        return PinWright::IsGameProcess() ? PinWright::GetTargetWorld() : PinWright::GetEditorWorldOrNull();
    }

    static AActor* FindActorByName(UWorld* World, const FString& ActorName)
    {
        return McpActorUtils::FindActorByNameSimple(World, ActorName);
    }

    // Same spelling as the editor file's copy (networking.get_networking_info `role`).
    static FString NetRoleToString(ENetRole Role)
    {
        switch (Role)
        {
            case ROLE_None: return TEXT("ROLE_None");
            case ROLE_SimulatedProxy: return TEXT("ROLE_SimulatedProxy");
            case ROLE_AutonomousProxy: return TEXT("ROLE_AutonomousProxy");
            case ROLE_Authority: return TEXT("ROLE_Authority");
            default: return TEXT("ROLE_Unknown");
        }
    }
}

// ---------------------------------------------------------------------------
// networking.set_owner
// ---------------------------------------------------------------------------
REGISTER_RPC_HANDLER("networking.set_owner", "networking", "Set or clear the owner of an actor in the world",
    RPC_PARAMS(
        RPC_PARAM_REQ("actorName", "string", "Name of the actor"),
        RPC_PARAM_OPT("ownerActorName", "string", "Name of the owner actor (empty to clear)")
    ))
{
    using namespace NetworkingHelpersNew;

    FString ActorName;
    if (!Ctx.RequireString(TEXT("actorName"), ActorName)) return true;
    FString OwnerActorName = Ctx.GetString(TEXT("ownerActorName"));

    UWorld* World = ResolveNetworkingWorld();
    if (!World) { Ctx.SendError(TEXT("NO_WORLD"), TEXT("No world available")); return true; }

    AActor* Actor = FindActorByName(World, ActorName);
    if (!Actor) { Ctx.SendError(TEXT("NOT_FOUND"), TEXT("Actor not found")); return true; }

    AActor* Owner = nullptr;
    if (!OwnerActorName.IsEmpty()) Owner = FindActorByName(World, OwnerActorName);

    Actor->SetOwner(Owner);

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetBoolField(TEXT("success"), true);
    Resp->SetStringField(TEXT("message"), Owner ? FString::Printf(TEXT("Set owner of %s to %s"), *ActorName, *OwnerActorName) : FString::Printf(TEXT("Cleared owner of %s"), *ActorName));
    AddActorVerification(Resp, Actor);
    Ctx.SendSuccess(Resp);
    return true;
}

// ---------------------------------------------------------------------------
// networking.check_has_authority
// ---------------------------------------------------------------------------
REGISTER_RPC_HANDLER("networking.check_has_authority", "networking", "Check if an actor has authority",
    RPC_PARAMS(
        RPC_PARAM_REQ("actorName", "string", "Name of the actor to check")
    ))
{
    using namespace NetworkingHelpersNew;

    FString ActorName;
    if (!Ctx.RequireString(TEXT("actorName"), ActorName)) return true;

    UWorld* World = ResolveNetworkingWorld();
    if (!World) { Ctx.SendError(TEXT("NO_WORLD"), TEXT("No world available")); return true; }

    AActor* Actor = FindActorByName(World, ActorName);
    if (!Actor) { Ctx.SendError(TEXT("NOT_FOUND"), TEXT("Actor not found")); return true; }

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetBoolField(TEXT("success"), true);
    Resp->SetBoolField(TEXT("hasAuthority"), Actor->HasAuthority());
    Resp->SetStringField(TEXT("role"), NetRoleToString(Actor->GetLocalRole()));
    Ctx.SendSuccess(Resp);
    return true;
}

// ---------------------------------------------------------------------------
// networking.check_is_locally_controlled
// ---------------------------------------------------------------------------
REGISTER_RPC_HANDLER("networking.check_is_locally_controlled", "networking", "Check if an actor/pawn is locally controlled",
    RPC_PARAMS(
        RPC_PARAM_REQ("actorName", "string", "Name of the actor to check")
    ))
{
    using namespace NetworkingHelpersNew;

    FString ActorName;
    if (!Ctx.RequireString(TEXT("actorName"), ActorName)) return true;

    UWorld* World = ResolveNetworkingWorld();
    if (!World) { Ctx.SendError(TEXT("NO_WORLD"), TEXT("No world available")); return true; }

    AActor* Actor = FindActorByName(World, ActorName);
    if (!Actor) { Ctx.SendError(TEXT("NOT_FOUND"), TEXT("Actor not found")); return true; }

    bool bIsLocallyControlled = false;
    bool bIsLocalController = false;
    APawn* Pawn = Cast<APawn>(Actor);
    if (Pawn)
    {
        bIsLocallyControlled = Pawn->IsLocallyControlled();
        APlayerController* PC = Cast<APlayerController>(Pawn->GetController());
        bIsLocalController = PC ? PC->IsLocalController() : false;
    }

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetBoolField(TEXT("success"), true);
    Resp->SetBoolField(TEXT("isLocallyControlled"), bIsLocallyControlled);
    Resp->SetBoolField(TEXT("isLocalController"), bIsLocalController);
    Ctx.SendSuccess(Resp);
    return true;
}
