// Copyright (c) 2026 Alexander Penkin. MIT License.

// SystemInspectSingletonsHandler.cpp - Live PIE game-framework singleton accessors.
// Exposes the six standard live UObject singletons (GameInstance, GameMode,
// GameState, PlayerController, PlayerState, LocalPlayer) as typed JSON RPCs.
// Each takes the optional `world` selector of editor.console_command (Handlers/Editor/
// PieWorldSelector.h). Omitted, it resolves to the PIE authority (listen / dedicated server,
// or the sole standalone instance), else the editor world; GEditor->PlayWorld, the old
// default, is the LAST PIE world created, i.e. a client in a listen-server session
// (B-inspect-singletons-pick-client-world). Every success echoes world / worldPath / netMode.

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ParamSpec.h"
#include "Handlers/ErrorCodes.h"
#include "Handlers/Editor/PieWorldSelector.h"
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

namespace SystemInspectSingletons
{

struct FResolvedWorld
{
    UWorld* World = nullptr;
    FString Selector;          // echoed as `world`: the caller's selector, or "pie:N" / "editor" when omitted
    int32 PieInstance = INDEX_NONE;
    bool bDefaulted = false;
};

// Resolves the optional `world` selector. Sends INVALID_ARGUMENT / WORLD_NOT_FOUND and returns
// false for an unparseable or unmatched selector; otherwise returns true, with Out.World null
// only when there is no editor at all (each verb keeps its own no-world answer).
static bool ResolveWorld(FHandlerContext& Ctx, FResolvedWorld& Out)
{
    const FString Requested = Ctx.GetString(TEXT("world")).TrimStartAndEnd();
    const PieWorldSelector::FParsedSelector Selector = PieWorldSelector::Parse(Requested);
    if (Selector.Kind == PieWorldSelector::ESelectorKind::Invalid)
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, Selector.Error);
        return false;
    }
    Out.bDefaulted = Requested.IsEmpty();
    Out.Selector = Out.bDefaulted ? TEXT("editor") : Requested;
    if (!GEditor)
    {
        return true;
    }

    const TArray<PieWorldSelector::FPieContextInfo> Contexts = PieWorldSelector::GatherPieContexts();
    int32 Match = INDEX_NONE;
    if (Out.bDefaulted)
    {
        Match = PieWorldSelector::ResolveOmittedForRead(Contexts);
    }
    else if (Selector.Kind != PieWorldSelector::ESelectorKind::Editor)
    {
        Match = PieWorldSelector::ResolveSelector(Selector, Contexts);
        if (Match == INDEX_NONE)
        {
            Ctx.SendError(ErrorCodes::ERR_WORLD_NOT_FOUND,
                FString::Printf(TEXT("No PIE world matches selector '%s'. Available PIE contexts: %s"),
                    *Requested, *PieWorldSelector::DescribeContexts(Contexts)));
            return false;
        }
    }

    if (Match == INDEX_NONE)
    {
        Out.World = GEditor->GetEditorWorldContext().World();
        return true;
    }
    Out.World = Contexts[Match].World;
    Out.PieInstance = Contexts[Match].PieInstance;
    if (Out.bDefaulted)
    {
        Out.Selector = FString::Printf(TEXT("pie:%d"), Out.PieInstance);
    }
    return true;
}

// Names the instance that answered, in editor.console_command's vocabulary plus netMode.
static void AddWorldFields(const FResolvedWorld& Resolved, FJsonObject& Resp)
{
    Resp.SetStringField(TEXT("world"), Resolved.Selector);
    Resp.SetBoolField(TEXT("worldDefaulted"), Resolved.bDefaulted);
    if (Resolved.PieInstance != INDEX_NONE)
    {
        Resp.SetNumberField(TEXT("pieInstance"), Resolved.PieInstance);
    }
    if (Resolved.World)
    {
        const ENetMode NetMode = Resolved.World->GetNetMode();
        Resp.SetStringField(TEXT("worldPath"), Resolved.World->GetPathName());
        Resp.SetStringField(TEXT("netMode"), PieWorldSelector::NetModeToString(NetMode));
        Resp.SetStringField(TEXT("kind"), PieWorldSelector::ClassifyNetMode(NetMode));
    }
}

} // namespace SystemInspectSingletons

#define PW_SINGLETON_WORLD_PARAM RPC_PARAM_OPT("world", "string", \
    "Which world answers, in editor.console_command's selector grammar: 'editor', 'server' (first PIE world with authority), 'client' / 'client:N' (N-th PIE client, 1-based) or 'pie:N' (raw PIEInstance). Omitted: the PIE authority (listen/dedicated server, or the sole standalone instance) while PIE runs, else the editor world. The response echoes world, worldDefaulted, pieInstance, worldPath, netMode and kind.")


// ---- system.inspect.get_game_instance ----
REGISTER_RPC_HANDLER("system.inspect.get_game_instance", "system.inspect",
    "Return the live UGameInstance for the selected world (see `world`) as { objectPath, className }.",
    RPC_PARAMS(PW_SINGLETON_WORLD_PARAM))
{
    SystemInspectSingletons::FResolvedWorld Resolved;
    if (!SystemInspectSingletons::ResolveWorld(Ctx, Resolved))
    {
        return true;
    }
    UWorld* World = Resolved.World;
    if (!World)
    {
        Ctx.SendError(ErrorCodes::ERR_GAME_INSTANCE_NOT_FOUND, TEXT("No active world."));
        return true;
    }
    UGameInstance* GI = World->GetGameInstance();
    if (!GI)
    {
        Ctx.SendError(ErrorCodes::ERR_GAME_INSTANCE_NOT_FOUND, FString::Printf(TEXT("World '%s' has no GameInstance."), *Resolved.Selector));
        return true;
    }
    TSharedPtr<FJsonObject> Result = EmitObjectRef(GI);
    SystemInspectSingletons::AddWorldFields(Resolved, *Result);
    Ctx.SendSuccess(Result);
    return true;
}

// ---- system.inspect.get_game_mode ----
REGISTER_RPC_HANDLER("system.inspect.get_game_mode", "system.inspect",
    "Return the live authoritative AGameModeBase for the selected world (see `world`) as { objectPath, className }.",
    RPC_PARAMS(PW_SINGLETON_WORLD_PARAM))
{
    SystemInspectSingletons::FResolvedWorld Resolved;
    if (!SystemInspectSingletons::ResolveWorld(Ctx, Resolved))
    {
        return true;
    }
    UWorld* World = Resolved.World;
    if (!World)
    {
        Ctx.SendError(ErrorCodes::ERR_GAME_MODE_NOT_FOUND, TEXT("No active world."));
        return true;
    }
    AGameModeBase* GM = World->GetAuthGameMode();
    if (!GM)
    {
        Ctx.SendError(ErrorCodes::ERR_GAME_MODE_NOT_FOUND, FString::Printf(TEXT("World '%s' has no authoritative GameMode (a PIE client world never has one; use world 'server')."), *Resolved.Selector));
        return true;
    }
    TSharedPtr<FJsonObject> Result = EmitObjectRef(GM);
    SystemInspectSingletons::AddWorldFields(Resolved, *Result);
    Ctx.SendSuccess(Result);
    return true;
}

// ---- system.inspect.get_game_state ----
REGISTER_RPC_HANDLER("system.inspect.get_game_state", "system.inspect",
    "Return the live AGameStateBase for the selected world (see `world`) as { objectPath, className }.",
    RPC_PARAMS(PW_SINGLETON_WORLD_PARAM))
{
    SystemInspectSingletons::FResolvedWorld Resolved;
    if (!SystemInspectSingletons::ResolveWorld(Ctx, Resolved))
    {
        return true;
    }
    UWorld* World = Resolved.World;
    if (!World)
    {
        Ctx.SendError(ErrorCodes::ERR_GAME_STATE_NOT_FOUND, TEXT("No active world."));
        return true;
    }
    AGameStateBase* GS = World->GetGameState();
    if (!GS)
    {
        Ctx.SendError(ErrorCodes::ERR_GAME_STATE_NOT_FOUND, FString::Printf(TEXT("World '%s' has no GameState."), *Resolved.Selector));
        return true;
    }
    TSharedPtr<FJsonObject> Result = EmitObjectRef(GS);
    SystemInspectSingletons::AddWorldFields(Resolved, *Result);
    Ctx.SendSuccess(Result);
    return true;
}

// ---- system.inspect.get_player_controllers ----
REGISTER_RPC_HANDLER("system.inspect.get_player_controllers", "system.inspect",
    "Return the live APlayerControllers for the selected world (see `world`) as an array of "
    "{ objectPath, className, playerIndex }. Empty array if no world / no PIE.",
    RPC_PARAMS(PW_SINGLETON_WORLD_PARAM))
{
    SystemInspectSingletons::FResolvedWorld Resolved;
    if (!SystemInspectSingletons::ResolveWorld(Ctx, Resolved))
    {
        return true;
    }
    UWorld* World = Resolved.World;
    TArray<TSharedPtr<FJsonValue>> Entries;
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
    SystemInspectSingletons::AddWorldFields(Resolved, *Result);
    Result->SetArrayField(TEXT("playerControllers"), Entries);
    Ctx.SendSuccess(Result);
    return true;
}

// ---- system.inspect.get_player_states ----
REGISTER_RPC_HANDLER("system.inspect.get_player_states", "system.inspect",
    "Return the live APlayerStates for the selected world (see `world`) as an array of "
    "{ objectPath, className, playerIndex }. Empty array if no world / no GameState.",
    RPC_PARAMS(PW_SINGLETON_WORLD_PARAM))
{
    SystemInspectSingletons::FResolvedWorld Resolved;
    if (!SystemInspectSingletons::ResolveWorld(Ctx, Resolved))
    {
        return true;
    }
    UWorld* World = Resolved.World;
    TArray<TSharedPtr<FJsonValue>> Entries;
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
    SystemInspectSingletons::AddWorldFields(Resolved, *Result);
    Result->SetArrayField(TEXT("playerStates"), Entries);
    Ctx.SendSuccess(Result);
    return true;
}

// ---- system.inspect.get_local_players ----
REGISTER_RPC_HANDLER("system.inspect.get_local_players", "system.inspect",
    "Return the live ULocalPlayers for the selected world (see `world`) as an array of "
    "{ objectPath, className, playerIndex, controllerId }. Empty array if no world / no GameInstance.",
    RPC_PARAMS(PW_SINGLETON_WORLD_PARAM))
{
    SystemInspectSingletons::FResolvedWorld Resolved;
    if (!SystemInspectSingletons::ResolveWorld(Ctx, Resolved))
    {
        return true;
    }
    UWorld* World = Resolved.World;
    TArray<TSharedPtr<FJsonValue>> Entries;
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
    SystemInspectSingletons::AddWorldFields(Resolved, *Result);
    Result->SetArrayField(TEXT("localPlayers"), Entries);
    Ctx.SendSuccess(Result);
    return true;
}

#undef PW_SINGLETON_WORLD_PARAM
