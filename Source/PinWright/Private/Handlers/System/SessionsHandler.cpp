// Copyright (c) 2026 Alexander Penkin. MIT License.

// SessionsHandler.cpp - Migrated from PinWright_SessionsHandlers.cpp
// Local multiplayer & PIE session testing: local player add/remove, LAN listen-server
// hosting via ServerTravel, and live session/multiplayer status readback.

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ErrorCodes.h"
#include "Handlers/System/SessionsTravelDecision.h"
#include "PinWrightHelpers.h"
#include "PinWrightSubsystem.h"
#include "State/PluginState.h"
#include "Utils/PieState.h"

#include "Editor.h"
#include "Engine/World.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/LocalPlayer.h"
#include "GameFramework/PlayerController.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"


DEFINE_LOG_CATEGORY_STATIC(LogMcpSessionsHandlers, Log, All);

namespace SessionsHelpers
{
    constexpr double GServerTravelTimeoutSeconds = 15.0;

    struct FServerTravelWaitState
    {
        TSharedPtr<PinWrightPieState::FPieLifecycleWaitControl> WaitControl;
        PinWrightSessionTravel::FServerTravelTerminalResponseGate ResponseGate;
        PinWrightSessionTravel::EServerTravelCompletionState CompletionState =
            PinWrightSessionTravel::EServerTravelCompletionState::Waiting;
    };

    UWorld* ResolveInitialServerTravelWorld()
    {
        if (!GEditor)
            return nullptr;
        if (GEditor->PlayWorld)
            return GEditor->PlayWorld;
        return GEditor->GetEditorWorldContext().World();
    }

    FString NormalizeMapPackagePath(const FString& MapPath)
    {
        return UWorld::RemovePIEPrefix(FPackageName::ObjectPathToPackageName(MapPath));
    }

    PinWrightPieState::FPieLifecycleState CaptureServerTravelCompletionState(
        const FName InitiatingWorldContextHandle,
        const FString& RequestedMapPackagePath,
        const uint32 InitialWorldUniqueID,
        const TSharedRef<FServerTravelWaitState>& WaitState)
    {
        const FWorldContext* InitiatingWorldContext = GEngine
            ? GEngine->GetWorldContextFromHandle(InitiatingWorldContextHandle)
            : nullptr;
        UWorld* CurrentWorld = InitiatingWorldContext
            ? InitiatingWorldContext->World()
            : nullptr;
        const bool bTravelPending = CurrentWorld
            && (!CurrentWorld->NextURL.IsEmpty() || CurrentWorld->IsInSeamlessTravel());

        PinWrightSessionTravel::FServerTravelCompletionObservation Observation;
        Observation.InitiatingContextHandle = InitiatingWorldContextHandle;
        Observation.ObservedContextHandle = InitiatingWorldContext
            ? InitiatingWorldContext->ContextHandle
            : NAME_None;
        Observation.InitialWorldUniqueID = InitialWorldUniqueID;
        Observation.ObservedWorldUniqueID = CurrentWorld ? CurrentWorld->GetUniqueID() : 0;
        Observation.RequestedMapPackagePath = RequestedMapPackagePath;
        Observation.bWorldAvailable = CurrentWorld != nullptr;
        Observation.bTravelPending = bTravelPending;
        if (CurrentWorld
            && Observation.ObservedWorldUniqueID != InitialWorldUniqueID
            && !bTravelPending)
        {
            Observation.ObservedMapPackagePath =
                NormalizeMapPackagePath(CurrentWorld->GetOutermost()->GetName());
        }
        WaitState->CompletionState =
            PinWrightSessionTravel::EvaluateServerTravelCompletionState(Observation);

        // The shared bounded waiter completes on bPieActive. For this injected travel probe that
        // bit represents the terminal map readback, not merely the presence of a PIE world.
        return {
            WaitState->CompletionState ==
                PinWrightSessionTravel::EServerTravelCompletionState::Completed,
            WaitState->CompletionState !=
                PinWrightSessionTravel::EServerTravelCompletionState::ContextLost,
            bTravelPending,
        };
    }

    void ApplyServerTravelOutcomeFields(
        const TSharedPtr<FJsonObject>& Response,
        const PinWrightSessionTravel::FServerTravelOutcome& Outcome)
    {
        Response->SetBoolField(TEXT("travelAccepted"), Outcome.bAccepted);
        Response->SetBoolField(TEXT("travelQueued"), Outcome.bQueued);
        Response->SetBoolField(TEXT("travelCompleted"), Outcome.bCompleted);
        Response->SetBoolField(TEXT("travelExecuted"), Outcome.bCompleted);
    }

    void ReleaseDispatcherLifetime(
        const TSharedPtr<FAsyncRequestLifetimeLease>& LifetimeLease)
    {
        if (LifetimeLease.IsValid())
            LifetimeLease->Release();
    }

    void AbandonServerTravelWait(
        const TSharedRef<FServerTravelWaitState>& WaitState,
        const TSharedRef<FAsyncResponseToken>& Token,
        const TSharedPtr<FJsonObject>& Response)
    {
        if (WaitState->WaitControl.IsValid())
            WaitState->WaitControl->Cancel();

        WaitState->ResponseGate.TryRespond(
            [Token, Response]()
            {
                Response->SetStringField(TEXT("status"), TEXT("server travel wait abandoned"));
                Response->SetBoolField(TEXT("travelCompleted"), false);
                Response->SetBoolField(TEXT("travelExecuted"), false);
                Response->SetBoolField(TEXT("timedOut"), false);
                Response->SetBoolField(TEXT("dispatcherEnded"), true);
                Token->SendError(ErrorCodes::ERR_HOST_FAILED,
                    TEXT("Dispatcher ended while waiting for server travel to complete"),
                    Response);
            });
    }

    UGameInstance* GetGameInstance()
    {
        if (GEditor && GEditor->PlayWorld)
            return GEditor->PlayWorld->GetGameInstance();
        return nullptr;
    }

    ULocalPlayer* GetLocalPlayerByIndex(int32 PlayerIndex)
    {
        UGameInstance* GI = GetGameInstance();
        if (GI)
        {
            const TArray<ULocalPlayer*>& LocalPlayers = GI->GetLocalPlayers();
            if (LocalPlayers.IsValidIndex(PlayerIndex))
                return LocalPlayers[PlayerIndex];
        }
        return nullptr;
    }

    int32 GetLocalPlayerCount()
    {
        UGameInstance* GI = GetGameInstance();
        if (GI)
            return GI->GetLocalPlayers().Num();
        return 0;
    }

    // Append one travel-option fragment onto a travel URL, honoring the UE
    // contract that a '?' must separate the first option from the preceding
    // token. The caller passes the bare option (e.g. "bClientReady=1" or
    // "Password=secret"); this guarantees exactly one leading '?' regardless of
    // whether the option already carries one. Centralizes the separator rule so
    // every handler-injected and user-supplied option fragment joins the same way.
    FString AppendTravelOption(const FString& BaseUrl, const FString& Option)
    {
        if (Option.IsEmpty())
            return BaseUrl;
        const FString Trimmed = Option.StartsWith(TEXT("?")) ? Option.RightChop(1) : Option;
        return BaseUrl + TEXT("?") + Trimmed;
    }
}

// ============================================================================
// Local Multiplayer
// ============================================================================

// ---- session.add_local_player ----
REGISTER_RPC_HANDLER("session.add_local_player", "session", "Add a local player to the active game instance",
    RPC_PARAMS(
        RPC_PARAM_OPT("controllerId", "number", "Controller ID (-1 for auto)")
    ))
{
    using namespace SessionsHelpers;

    int32 ControllerId = Ctx.GetInt(TEXT("controllerId"), -1);

    UGameInstance* GI = GetGameInstance();
    if (!GI)
    {
        Ctx.SendError(ErrorCodes::ERR_NO_GAME_INSTANCE, TEXT("No active game instance. Start Play-In-Editor first."));
        return true;
    }

    FString Error;
    ULocalPlayer* NewPlayer = GI->CreateLocalPlayer(ControllerId, Error, true);
    if (!NewPlayer)
    {
        Ctx.SendError(ErrorCodes::ERR_ADD_PLAYER_FAILED,
            FString::Printf(TEXT("Failed to add local player: %s"), *Error));
        return true;
    }

    int32 PlayerIndex = GI->GetLocalPlayers().Find(NewPlayer);

    TSharedPtr<FJsonObject> ResponseJson = MakeShared<FJsonObject>();
    ResponseJson->SetNumberField(TEXT("playerIndex"), PlayerIndex);
    ResponseJson->SetNumberField(TEXT("controllerId"), ControllerId);
    ResponseJson->SetNumberField(TEXT("totalLocalPlayers"), GI->GetLocalPlayers().Num());
    Ctx.SendSuccess(ResponseJson);
    return true;
}

// ---- session.remove_local_player ----
REGISTER_RPC_HANDLER("session.remove_local_player", "session", "Remove a local player from the active game instance",
    RPC_PARAMS(
        RPC_PARAM_REQ("playerIndex", "integer", "Player index to remove (cannot be 0)")
    ))
{
    using namespace SessionsHelpers;

    int32 PlayerIndex = -1;
    if (!Ctx.RequireInt(TEXT("playerIndex"), PlayerIndex))
    {
        return true;
    }

    UGameInstance* GI = GetGameInstance();
    if (!GI)
    {
        Ctx.SendError(ErrorCodes::ERR_NO_GAME_INSTANCE, TEXT("No active game instance. Start Play-In-Editor first."));
        return true;
    }

    if (PlayerIndex == 0)
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, TEXT("Cannot remove the primary local player (index 0)."));
        return true;
    }

    ULocalPlayer* Player = GetLocalPlayerByIndex(PlayerIndex);
    if (!Player)
    {
        Ctx.SendError(ErrorCodes::ERR_PLAYER_NOT_FOUND,
            FString::Printf(TEXT("No local player at index %d"), PlayerIndex));
        return true;
    }

    GI->RemoveLocalPlayer(Player);

    TSharedPtr<FJsonObject> ResponseJson = MakeShared<FJsonObject>();
    ResponseJson->SetNumberField(TEXT("removedPlayerIndex"), PlayerIndex);
    ResponseJson->SetNumberField(TEXT("remainingPlayers"), GI->GetLocalPlayers().Num());
    Ctx.SendSuccess(ResponseJson);
    return true;
}

// ============================================================================
// LAN
// ============================================================================

// ---- session.host_lan_server ----
REGISTER_RPC_HANDLER("session.host_lan_server", "session", "Host a LAN server on a specified map",
    RPC_PARAMS(
        RPC_PARAM_OPT("serverName", "string", "Server name"),
        RPC_PARAM_REQ("mapName", "path", "Map name or path"),
        RPC_PARAM_OPT("maxPlayers", "number", "Max players (default 4)"),
        RPC_PARAM_OPT("travelOptions", "string", "Additional travel options"),
        RPC_PARAM_OPT("executeTravel", "boolean", "Execute server travel immediately")
    ))
{
    FString ServerName = Ctx.GetString(TEXT("serverName"), TEXT("LAN Server"));
    FString MapName = Ctx.GetString(TEXT("mapName"));
    int32 MaxPlayers = Ctx.GetInt(TEXT("maxPlayers"), 4);
    FString TravelOptions = Ctx.GetString(TEXT("travelOptions"));
    bool bExecuteTravel = Ctx.GetBool(TEXT("executeTravel"), false);

    if (MapName.IsEmpty())
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, TEXT("mapName is required to host a LAN server"));
        return true;
    }

    FString FullTravelOptions = FString::Printf(TEXT("?listen?bIsLanMatch=1?MaxPlayers=%d"), MaxPlayers);
    // Join user travelOptions through the same separator-aware helper so a bare
    // option (e.g. "bClientReady=1") gets its required '?' instead of fusing onto
    // the preceding "MaxPlayers=%d" value.
    FullTravelOptions = SessionsHelpers::AppendTravelOption(FullTravelOptions, TravelOptions);

    FString FullMapPath = MapName;
    if (!IsValidMountPoint(FullMapPath) && !FullMapPath.StartsWith(TEXT("/")) && !FullMapPath.Contains(TEXT(":")))
        FullMapPath = FString::Printf(TEXT("/Game/%s"), *MapName);

    const FString TravelURL = FullMapPath + FullTravelOptions;
    TSharedPtr<FJsonObject> ResponseJson = MakeShared<FJsonObject>();
    ResponseJson->SetStringField(TEXT("serverName"), ServerName);
    ResponseJson->SetStringField(TEXT("mapName"), MapName);
    ResponseJson->SetStringField(TEXT("mapPath"), FullMapPath);
    ResponseJson->SetNumberField(TEXT("maxPlayers"), MaxPlayers);
    ResponseJson->SetStringField(TEXT("travelURL"), TravelURL);
    ResponseJson->SetStringField(TEXT("status"), TEXT("configured"));
    ResponseJson->SetBoolField(TEXT("timedOut"), false);
    SessionsHelpers::ApplyServerTravelOutcomeFields(ResponseJson, {});

    if (!bExecuteTravel)
    {
        Ctx.SendSuccess(ResponseJson);
        return true;
    }

    UWorld* World = SessionsHelpers::ResolveInitialServerTravelWorld();
    if (!World)
    {
        const FString StatusMessage =
            TEXT("No world available. Start Play-In-Editor first to execute travel.");
        ResponseJson->SetStringField(TEXT("status"), StatusMessage);
        Ctx.SendError(ErrorCodes::ERR_HOST_FAILED, StatusMessage, ResponseJson);
        return true;
    }

    const FWorldContext* InitiatingWorldContext = GEngine
        ? GEngine->GetWorldContextFromWorld(World)
        : nullptr;
    if (!InitiatingWorldContext || InitiatingWorldContext->ContextHandle.IsNone())
    {
        const FString StatusMessage = TEXT("No world context available for server travel.");
        ResponseJson->SetStringField(TEXT("status"), StatusMessage);
        Ctx.SendError(ErrorCodes::ERR_HOST_FAILED, StatusMessage, ResponseJson);
        return true;
    }
    const FName InitiatingWorldContextHandle = InitiatingWorldContext->ContextHandle;

    const uint32 InitialWorldUniqueID = World->GetUniqueID();
    const PinWrightSessionTravel::FServerTravelPendingState PendingBefore = {
        World->NextURL,
        World->IsInSeamlessTravel(),
    };
    const bool bServerTravelAccepted = World->ServerTravel(TravelURL, true);
    const PinWrightSessionTravel::FServerTravelPendingState PendingAfter = {
        World->NextURL,
        World->IsInSeamlessTravel(),
    };
    const PinWrightSessionTravel::FServerTravelOutcome TravelOutcome =
        PinWrightSessionTravel::EvaluateServerTravelOutcome(
            bServerTravelAccepted,
            PendingBefore,
            PendingAfter,
            TravelURL,
            false);
    SessionsHelpers::ApplyServerTravelOutcomeFields(ResponseJson, TravelOutcome);

    if (TravelOutcome.IsRefused())
    {
        const FString RefusalMessage = TravelOutcome.bAccepted
            ? TEXT("ServerTravel accepted the call but did not newly queue the requested destination.")
            : TEXT("ServerTravel refused the requested destination.");
        ResponseJson->SetStringField(TEXT("status"), TEXT("server travel refused"));
        UE_LOG(LogMcpSessionsHandlers, Warning, TEXT("LAN Server: %s URL=%s"),
            *RefusalMessage, *TravelURL);
        Ctx.SendError(ErrorCodes::ERR_TRAVEL_REFUSED, RefusalMessage, ResponseJson);
        return true;
    }

    UE_LOG(LogMcpSessionsHandlers, Log, TEXT("LAN Server: Queued ServerTravel to %s"), *TravelURL);

    const FString RequestedMapPackagePath =
        SessionsHelpers::NormalizeMapPackagePath(FullMapPath);
    const TSharedRef<FAsyncResponseToken> Token = Ctx.MakeAsyncToken();
    const TSharedRef<SessionsHelpers::FServerTravelWaitState> WaitState =
        MakeShared<SessionsHelpers::FServerTravelWaitState>();
    const TSharedPtr<FAsyncRequestLifetimeLease> LifetimeLease =
        Ctx.RetainAsyncRequestLifetime(
            [WaitState, Token, ResponseJson]()
            {
                SessionsHelpers::AbandonServerTravelWait(WaitState, Token, ResponseJson);
            });

    WaitState->WaitControl = PinWrightPieState::WaitForPieLifecycleState(
        PinWrightPieState::EPieLifecycleWaitTarget::PlayWorldActive,
        SessionsHelpers::GServerTravelTimeoutSeconds,
        [Token, ResponseJson, WaitState, LifetimeLease, bServerTravelAccepted,
         PendingBefore, PendingAfter, TravelURL](
            const PinWrightPieState::FPieLifecycleWaitResult& WaitOutcome)
        {
            WaitState->ResponseGate.TryRespond(
                [Token, ResponseJson, WaitState, LifetimeLease, bServerTravelAccepted,
                 PendingBefore, PendingAfter, TravelURL, WaitOutcome]()
                {
                    SessionsHelpers::ReleaseDispatcherLifetime(LifetimeLease);
                    const bool bDestinationObserved = WaitState->CompletionState ==
                        PinWrightSessionTravel::EServerTravelCompletionState::Completed;
                    const PinWrightSessionTravel::FServerTravelOutcome FinalOutcome =
                        PinWrightSessionTravel::EvaluateServerTravelOutcome(
                            bServerTravelAccepted,
                            PendingBefore,
                            PendingAfter,
                            TravelURL,
                            bDestinationObserved);
                    SessionsHelpers::ApplyServerTravelOutcomeFields(ResponseJson, FinalOutcome);
                    ResponseJson->SetBoolField(TEXT("timedOut"), WaitOutcome.bTimedOut);

                    if (WaitState->CompletionState ==
                        PinWrightSessionTravel::EServerTravelCompletionState::ContextLost)
                    {
                        ResponseJson->SetStringField(
                            TEXT("status"), TEXT("server travel context lost"));
                        Token->SendError(ErrorCodes::ERR_HOST_FAILED,
                            TEXT("The initiating world context disappeared while waiting for server travel to complete"),
                            ResponseJson);
                        return;
                    }

                    if (!FinalOutcome.bCompleted)
                    {
                        ResponseJson->SetStringField(
                            TEXT("status"), TEXT("server travel timed out"));
                        Token->SendError(ErrorCodes::ERR_HOST_FAILED,
                            TEXT("Server travel did not reach the requested destination within 15 seconds"),
                            ResponseJson);
                        return;
                    }

                    ResponseJson->SetStringField(TEXT("status"), TEXT("server travel completed"));
                    UE_LOG(LogMcpSessionsHandlers, Log,
                        TEXT("LAN Server: ServerTravel completed at %s"), *TravelURL);
                    Token->SendSuccess(ResponseJson);
                });
        },
        [InitiatingWorldContextHandle, RequestedMapPackagePath,
         InitialWorldUniqueID, WaitState]()
        {
            return SessionsHelpers::CaptureServerTravelCompletionState(
                InitiatingWorldContextHandle,
                RequestedMapPackagePath,
                InitialWorldUniqueID,
                WaitState);
        },
        [WaitState]()
        {
            return WaitState->CompletionState ==
                PinWrightSessionTravel::EServerTravelCompletionState::ContextLost;
        });
    return true;
}

// ---- session.get_sessions_info ----
REGISTER_RPC_HANDLER("session.get_sessions_info", "session", "Get current session and multiplayer status information",
    RPC_NO_PARAMS)
{
    using namespace SessionsHelpers;

    TSharedPtr<FJsonObject> ResponseJson = MakeShared<FJsonObject>();
    TSharedPtr<FJsonObject> SessionsInfo = MakeShared<FJsonObject>();

    int32 LocalPlayerCount = GetLocalPlayerCount();
    SessionsInfo->SetNumberField(TEXT("localPlayerCount"), LocalPlayerCount);

    bool bInPIE = GEditor && GEditor->PlayWorld != nullptr;
    SessionsInfo->SetBoolField(TEXT("inPlaySession"), bInPIE);

    SessionsInfo->SetNumberField(TEXT("currentPlayers"), LocalPlayerCount);
    SessionsInfo->SetBoolField(TEXT("splitScreenEnabled"), LocalPlayerCount > 1);
    // splitScreenType is a live readback of the current local-player roster:
    // "Active" once more than one local player exists (split-screen in effect),
    // "None" otherwise.
    SessionsInfo->SetStringField(TEXT("splitScreenType"),
        LocalPlayerCount > 1 ? TEXT("Active") : TEXT("None"));

    SessionsInfo->SetBoolField(TEXT("isHosting"), false);
    SessionsInfo->SetStringField(TEXT("connectedServerAddress"), TEXT(""));

    ResponseJson->SetObjectField(TEXT("sessionsInfo"), SessionsInfo);
    Ctx.SendSuccess(ResponseJson);
    return true;
}
