// Copyright (c) 2026 Alexander Penkin. MIT License.

// SystemInspectSingletonsHandler.cpp - Live PIE game-framework singleton accessors.
// Exposes the six standard live UObject singletons (GameInstance, GameMode,
// GameState, PlayerController, PlayerState, LocalPlayer) as typed JSON RPCs.
// World resolution is PIE-first via McpActorUtils::ResolveQueryWorld so the
// same RPC works during PIE and silently no-ops (empty array / typed *_NOT_FOUND
// error) outside it.

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ParamSpec.h"
#include "Utils/ActorUtils.h"
#include "Utils/JsonUtils.h"
#include "Dom/JsonObject.h"

#include "Editor.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Engine/GameInstance.h"
#include "Engine/LocalPlayer.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/GameStateBase.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerState.h"


// ---- system.inspect.get_game_instance ----
REGISTER_RPC_HANDLER("system.inspect.get_game_instance", "system.inspect",
    "Return the live UGameInstance for the active (PIE-first) world as { objectPath, className }.",
    RPC_NO_PARAMS)
{
    FString ResolvedMode;
    UWorld* World = McpActorUtils::ResolveQueryWorld(FString(), ResolvedMode);
    if (!World)
    {
        Ctx.SendError(TEXT("GAME_INSTANCE_NOT_FOUND"), TEXT("No active world."));
        return true;
    }
    UGameInstance* GI = World->GetGameInstance();
    if (!GI)
    {
        Ctx.SendError(TEXT("GAME_INSTANCE_NOT_FOUND"), TEXT("World has no GameInstance."));
        return true;
    }
    Ctx.SendSuccess(EmitObjectRef(GI));
    return true;
}

// ---- system.inspect.get_game_mode ----
REGISTER_RPC_HANDLER("system.inspect.get_game_mode", "system.inspect",
    "Return the live authoritative AGameModeBase for the active (PIE-first) world as { objectPath, className }.",
    RPC_NO_PARAMS)
{
    FString ResolvedMode;
    UWorld* World = McpActorUtils::ResolveQueryWorld(FString(), ResolvedMode);
    if (!World)
    {
        Ctx.SendError(TEXT("GAME_MODE_NOT_FOUND"), TEXT("No active world."));
        return true;
    }
    AGameModeBase* GM = World->GetAuthGameMode();
    if (!GM)
    {
        Ctx.SendError(TEXT("GAME_MODE_NOT_FOUND"), TEXT("World has no authoritative GameMode."));
        return true;
    }
    Ctx.SendSuccess(EmitObjectRef(GM));
    return true;
}

// ---- system.inspect.get_game_state ----
REGISTER_RPC_HANDLER("system.inspect.get_game_state", "system.inspect",
    "Return the live AGameStateBase for the active (PIE-first) world as { objectPath, className }.",
    RPC_NO_PARAMS)
{
    FString ResolvedMode;
    UWorld* World = McpActorUtils::ResolveQueryWorld(FString(), ResolvedMode);
    if (!World)
    {
        Ctx.SendError(TEXT("GAME_STATE_NOT_FOUND"), TEXT("No active world."));
        return true;
    }
    AGameStateBase* GS = World->GetGameState();
    if (!GS)
    {
        Ctx.SendError(TEXT("GAME_STATE_NOT_FOUND"), TEXT("World has no GameState."));
        return true;
    }
    Ctx.SendSuccess(EmitObjectRef(GS));
    return true;
}

// ---- system.inspect.get_player_controllers ----
REGISTER_RPC_HANDLER("system.inspect.get_player_controllers", "system.inspect",
    "Return the live APlayerControllers for the active (PIE-first) world as an array of "
    "{ objectPath, className, playerIndex }. Empty array if no world / no PIE.",
    RPC_NO_PARAMS)
{
    TArray<TSharedPtr<FJsonValue>> Entries;
    FString ResolvedMode;
    UWorld* World = McpActorUtils::ResolveQueryWorld(FString(), ResolvedMode);
    if (World)
    {
        int32 Index = 0;
        for (FConstPlayerControllerIterator It = World->GetPlayerControllerIterator(); It; ++It)
        {
            APlayerController* PC = It->Get();
            if (PC)
            {
                TSharedPtr<FJsonObject> Entry = EmitObjectRef(PC);
                Entry->SetNumberField(TEXT("playerIndex"), Index);
                Entries.Add(MakeShared<FJsonValueObject>(Entry));
            }
            ++Index;
        }
    }
    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetArrayField(TEXT("playerControllers"), Entries);
    Ctx.SendSuccess(Result);
    return true;
}

// ---- system.inspect.get_player_states ----
REGISTER_RPC_HANDLER("system.inspect.get_player_states", "system.inspect",
    "Return the live APlayerStates for the active (PIE-first) world as an array of "
    "{ objectPath, className, playerIndex }. Empty array if no world / no GameState.",
    RPC_NO_PARAMS)
{
    TArray<TSharedPtr<FJsonValue>> Entries;
    FString ResolvedMode;
    UWorld* World = McpActorUtils::ResolveQueryWorld(FString(), ResolvedMode);
    if (World)
    {
        AGameStateBase* GS = World->GetGameState();
        if (GS)
        {
            for (int32 Index = 0; Index < GS->PlayerArray.Num(); ++Index)
            {
                APlayerState* PS = GS->PlayerArray[Index];
                if (!PS)
                {
                    continue;
                }
                TSharedPtr<FJsonObject> Entry = EmitObjectRef(PS);
                Entry->SetNumberField(TEXT("playerIndex"), Index);
                Entries.Add(MakeShared<FJsonValueObject>(Entry));
            }
        }
    }
    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetArrayField(TEXT("playerStates"), Entries);
    Ctx.SendSuccess(Result);
    return true;
}

// ---- system.inspect.get_local_players ----
REGISTER_RPC_HANDLER("system.inspect.get_local_players", "system.inspect",
    "Return the live ULocalPlayers for the active (PIE-first) world as an array of "
    "{ objectPath, className, playerIndex, controllerId }. Empty array if no world / no GameInstance.",
    RPC_NO_PARAMS)
{
    TArray<TSharedPtr<FJsonValue>> Entries;
    FString ResolvedMode;
    UWorld* World = McpActorUtils::ResolveQueryWorld(FString(), ResolvedMode);
    if (World)
    {
        UGameInstance* GI = World->GetGameInstance();
        if (GI)
        {
            const TArray<ULocalPlayer*>& LocalPlayers = GI->GetLocalPlayers();
            for (int32 Index = 0; Index < LocalPlayers.Num(); ++Index)
            {
                ULocalPlayer* LP = LocalPlayers[Index];
                if (!LP)
                {
                    continue;
                }
                TSharedPtr<FJsonObject> Entry = EmitObjectRef(LP);
                Entry->SetNumberField(TEXT("playerIndex"), Index);
                Entry->SetNumberField(TEXT("controllerId"), LP->GetControllerId());
                Entries.Add(MakeShared<FJsonValueObject>(Entry));
            }
        }
    }
    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetArrayField(TEXT("localPlayers"), Entries);
    Ctx.SendSuccess(Result);
    return true;
}
