// Copyright (c) 2026 Alexander Penkin. MIT License.

// PIEHandler.cpp - Migrated from PinWright_ControlHandlers.cpp
// Handles editor.play, editor.stop, editor.pause, editor.resume,
// editor.step_frame, editor.eject, editor.possess, editor.status,
// editor.pie_status

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ErrorCodes.h"
#include "Handlers/ParamSpec.h"
#include "Handlers/ParamAliasUtils.h"
#include "PinWrightGlobals.h"
#include "PinWrightHelpers.h"
#include "PinWrightSubsystem.h"
#include "Utils/ActorUtils.h"
#include "Utils/PieState.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Handlers/Editor/PieWorldSelector.h"

#include "Editor.h"
#include "Engine/World.h"
#include "GameFramework/GameStateBase.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/Pawn.h"
#include "Modules/ModuleManager.h"
#if __has_include("Subsystems/EditorActorSubsystem.h")
#include "Subsystems/EditorActorSubsystem.h"
#elif __has_include("EditorActorSubsystem.h")
#include "EditorActorSubsystem.h"
#endif

// The PlayInEditor start recipe (params + play settings + first-active-viewport routing +
// the LevelEditor / play-settings __has_include gates) lives in this shared header
// (editor.play is its sole consumer since the ui.play_in_editor alias was removed).
#include "Handlers/Editor/PieControlUtils.h"
#include "Handlers/Editor/PieNetworkEmulation.h"
#include "GameFramework/Actor.h"

// The clock control behind pause / resume / step_frame. bDebugPauseExecution freezes the
// WORLD only; freezing and stepping the UI (UMG animations and widget Tick) needs the two
// separate levers this owns. Every engine citation for why is in its header.
#include "Handlers/Editor/PieTimeControl.h"

namespace
{
constexpr double GPieLifecycleTimeoutSeconds = 15.0;

// RPC lifecycle handlers and their ticker callbacks run on the editor thread. This owner makes
// that single-threaded assumption explicit and gives every waiter a distinct generation.
PinWrightPieState::FPieLifecycleOperationOwner GPieLifecycleOperationOwner;

struct FPieStopActions
{
  bool bStartRequestCancelled = false;
  bool bStartRequestWasQueued = false;
};

struct FPieDispatcherWaitState
{
  TSharedPtr<PinWrightPieState::FPieLifecycleWaitControl> WaitControl;
  bool bResponseSent = false;
};

const TCHAR* PieLifecycleOperationName(
    const PinWrightPieState::EPieLifecycleOperationType Type)
{
  switch (Type)
  {
  case PinWrightPieState::EPieLifecycleOperationType::Play:
    return TEXT("play");
  case PinWrightPieState::EPieLifecycleOperationType::Cleanup:
    return TEXT("cleanup");
  case PinWrightPieState::EPieLifecycleOperationType::Stop:
  default:
    return TEXT("stop");
  }
}

void ApplyPieStopAction(
    const PinWrightPieState::FPieLifecycleState& State,
    FPieStopActions& Actions)
{
  if (!GEditor)
  {
    return;
  }

  switch (PinWrightPieState::EvaluatePieStopAction(State))
  {
  case PinWrightPieState::EPieStopAction::RequestEndPlay:
    PinWrightPieTime::Release();
    if (!GEditor->ShouldEndPlayMap())
    {
      GEditor->RequestEndPlayMap();
    }
    break;
  case PinWrightPieState::EPieStopAction::CancelQueuedStartRequest:
    // UE 5.8 resets both PlaySessionRequest and PlayInEditorSessionInfo. Only call this while
    // the request is still queued; deferred startup callbacks may still use session info after
    // the request has been consumed and before PlayWorld appears.
    if (GEditor->IsPlaySessionRequestQueued())
    {
      Actions.bStartRequestWasQueued = true;
      GEditor->CancelRequestPlaySession();
      Actions.bStartRequestCancelled = true;
    }
    break;
  case PinWrightPieState::EPieStopAction::WaitForTransition:
  case PinWrightPieState::EPieStopAction::None:
  default:
    break;
  }
}

void AddPieStopActionFields(
    const TSharedPtr<FJsonObject>& Response,
    const FPieStopActions& Actions)
{
  const PinWrightPieState::FPieLifecycleState State =
      PinWrightPieState::CaptureEditorPieLifecycleState();
  const bool bTransitionCleanupPending =
      PinWrightPieState::EvaluatePieStopAction(State) ==
      PinWrightPieState::EPieStopAction::WaitForTransition;
  const bool bEndPlayRequested = GEditor && GEditor->ShouldEndPlayMap();
  Response->SetBoolField(TEXT("startRequestCancelled"), Actions.bStartRequestCancelled);
  Response->SetBoolField(TEXT("startRequestWasQueued"), Actions.bStartRequestWasQueued);
  Response->SetBoolField(TEXT("transitionCleanupPending"), bTransitionCleanupPending);
  Response->SetBoolField(TEXT("endPlayRequested"), bEndPlayRequested);
}

void ReleaseDispatcherLifetime(
    const TSharedPtr<FAsyncRequestLifetimeLease>& LifetimeLease)
{
  if (LifetimeLease.IsValid())
  {
    LifetimeLease->Release();
  }
}

void AbandonPieLifecycleWait(
    const TSharedRef<PinWrightPieState::FPieLifecycleOperation>& Operation,
    const TSharedRef<FPieDispatcherWaitState>& WaitState,
    const TSharedRef<FAsyncResponseToken>& Token,
    const TSharedPtr<FJsonObject>& Response,
    const TCHAR* ErrorCode,
    const TCHAR* Message,
    const TSharedPtr<FPieStopActions>& StopActions = nullptr)
{
  if (WaitState->WaitControl.IsValid())
  {
    WaitState->WaitControl->Cancel();
  }
  GPieLifecycleOperationOwner.Complete(Operation);

  if (WaitState->bResponseSent)
  {
    return;
  }
  WaitState->bResponseSent = true;

  const PinWrightPieState::FPieLifecycleState State =
      PinWrightPieState::CaptureEditorPieLifecycleState();
  Response->SetBoolField(TEXT("success"), false);
  Response->SetBoolField(TEXT("pieActive"), State.bPieActive);
  Response->SetBoolField(TEXT("sessionInProgress"), State.bSessionInProgress);
  Response->SetBoolField(TEXT("startRequestQueued"), State.bStartRequestQueued);
  Response->SetBoolField(TEXT("timedOut"), false);
  Response->SetBoolField(TEXT("cancelled"), true);
  Response->SetBoolField(TEXT("dispatcherEnded"), true);
  if (StopActions.IsValid())
  {
    AddPieStopActionFields(Response, *StopActions);
  }
  Token->SendError(ErrorCode, Message, Response);
}

PinWrightPieState::FPieLifecycleState CaptureAndApplyPieStopState(
    const TSharedRef<PinWrightPieState::FPieLifecycleOperation>& Operation,
    FPieStopActions& Actions)
{
  PinWrightPieState::FPieLifecycleState State =
      PinWrightPieState::CaptureEditorPieLifecycleState();
  // WaitForPieLifecycleState probes before it checks cancellation. Guard the action itself so a
  // stale cleanup generation can observe, but never stop, a later request's PlayWorld.
  if (!GPieLifecycleOperationOwner.IsCurrent(Operation))
  {
    return State;
  }

  ApplyPieStopAction(State, Actions);
  return PinWrightPieState::CaptureEditorPieLifecycleState();
}

void StartPieTimeoutCleanupWatcher(
    const TSharedRef<PinWrightPieState::FPieLifecycleOperation>& Operation,
    const TSharedRef<FPieStopActions>& Actions,
    const TSharedRef<FPieDispatcherWaitState>& WaitState,
    const TSharedPtr<FAsyncRequestLifetimeLease>& LifetimeLease)
{
  if (!GPieLifecycleOperationOwner.IsCurrent(Operation))
  {
    ReleaseDispatcherLifetime(LifetimeLease);
    return;
  }

  Operation->Type = PinWrightPieState::EPieLifecycleOperationType::Cleanup;
  WaitState->WaitControl = PinWrightPieState::WaitForPieLifecycleState(
      PinWrightPieState::EPieLifecycleWaitTarget::SessionInactive,
      GPieLifecycleTimeoutSeconds,
      [Operation, LifetimeLease](
          const PinWrightPieState::FPieLifecycleWaitResult& /*Outcome*/)
      {
        // Reached, timed out, or superseded: release only this generation. Complete is a no-op
        // if a later generation somehow became current first.
        GPieLifecycleOperationOwner.Complete(Operation);
        ReleaseDispatcherLifetime(LifetimeLease);
      },
      [Operation, Actions]()
      {
        return CaptureAndApplyPieStopState(Operation, *Actions);
      },
      [Operation]()
      {
        return !GPieLifecycleOperationOwner.IsCurrent(Operation);
      });
}
}

// ---- editor.play ----
REGISTER_RPC_HANDLER("editor.play", "editor", "Start a Play-In-Editor (PIE) session for the current level and answer only after the PIE world exists (or a bounded startup timeout). Idempotent: returns alreadyPlaying=true if a PIE session is already active. Uses the level editor's first active viewport as the destination if available. Optional numClients/netMode start a multi-instance networked session (netMode 'listen' + numClients N spawns N auto-connected PIE clients under one process) via a transient copy of the play settings - the user's saved Editor Preferences are never modified. PIE network emulation is FORCE-DISABLED for the session unless the optional networkEmulation param explicitly enables it (the saved 'Enable Network Emulation' editor preference is never inherited, so test sessions get a deterministic wire by default); the state in effect is echoed in the response and readable per-session via editor.pie_status.",
    RPC_PARAMS(
        RPC_PARAM_OPT("numClients", "number", "Number of PIE player instances to start (>=1). Omit to use the saved play settings."),
        RPC_PARAM_OPT("netMode", "string", "PIE net mode for this session: 'standalone', 'listen' (listen server, clients auto-connect), or 'client'. Omit to use the saved play settings."),
        RPC_PARAM_OPT("networkEmulation", "object", "Explicit PIE network-emulation control: {enabled?: bool, target?: string, profile?: string}. Omitted or enabled:false force-disables emulation for this session regardless of the user's saved play settings. enabled:true applies target 'serverOnly' (default) | 'clientsOnly' | 'everyone' and a Network Emulation Profile name from the project config (default 'Average'; stock list: Average, Bad, BufferBloat). An unknown target or profile fails with INVALID_ARGUMENT listing the valid values. Applied to a transient session copy only - never persisted to the user's settings.")
    ))
{
  if (!GEditor) {
    Ctx.SendError(ErrorCodes::ERR_EDITOR_NOT_AVAILABLE, TEXT("Editor not available"));
    return true;
  }

  if (const TSharedPtr<PinWrightPieState::FPieLifecycleOperation> ActiveOperation =
          GPieLifecycleOperationOwner.GetActiveOperation())
  {
    const PinWrightPieState::FPieLifecycleState State =
        PinWrightPieState::CaptureEditorPieLifecycleState();
    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetBoolField(TEXT("success"), false);
    Resp->SetBoolField(TEXT("pieActive"), State.bPieActive);
    Resp->SetBoolField(TEXT("sessionInProgress"), State.bSessionInProgress);
    Resp->SetBoolField(TEXT("startRequestQueued"), State.bStartRequestQueued);
    Resp->SetBoolField(TEXT("timedOut"), false);
    Resp->SetStringField(TEXT("activeLifecycleOperation"),
        PieLifecycleOperationName(ActiveOperation->Type));
    Ctx.SendError(ErrorCodes::ERR_PIE_START_IN_PROGRESS,
        TEXT("A PIE lifecycle operation is already in progress; wait for it to finish before starting another session"),
        Resp);
    return true;
  }

  if (GEditor->PlayWorld) {
    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetBoolField(TEXT("success"), true);
    Resp->SetBoolField(TEXT("alreadyPlaying"), true);
    Resp->SetBoolField(TEXT("pieActive"), true);
    Resp->SetBoolField(TEXT("sessionInProgress"), GEditor->IsPlaySessionInProgress());
    Resp->SetBoolField(TEXT("timedOut"), false);
    Ctx.SendSuccess(Resp);
    return true;
  }

  if (GEditor->IsPlaySessionInProgress()) {
    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetBoolField(TEXT("success"), false);
    Resp->SetBoolField(TEXT("pieActive"), false);
    Resp->SetBoolField(TEXT("sessionInProgress"), true);
    Resp->SetBoolField(TEXT("startRequestQueued"), GEditor->IsPlaySessionRequestQueued());
    Resp->SetBoolField(TEXT("timedOut"), false);
    Ctx.SendError(ErrorCodes::ERR_PIE_START_IN_PROGRESS,
        TEXT("A PIE start or session transition is already in progress; wait for it to finish or call editor.stop before retrying"),
        Resp);
    return true;
  }

  const int32 NumClients = Ctx.GetInt(TEXT("numClients"), 0);
  FString NetModeStr = Ctx.GetString(TEXT("netMode"));
  int32 NetMode = -1;
  if (!NetModeStr.IsEmpty()) {
    if (NetModeStr.Equals(TEXT("standalone"), ESearchCase::IgnoreCase)) {
      NetMode = 0;
    } else if (NetModeStr.Equals(TEXT("listen"), ESearchCase::IgnoreCase)) {
      NetMode = 1;
    } else if (NetModeStr.Equals(TEXT("client"), ESearchCase::IgnoreCase)) {
      NetMode = 2;
    } else {
      Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
          FString::Printf(TEXT("Unknown netMode '%s'. Valid values: standalone, listen, client"), *NetModeStr));
      return true;
    }
  }

  // Network emulation defaults to force-disabled (spec default) — the user's saved
  // "Enable Network Emulation" preference is deliberately NOT inherited. target/profile
  // are validated even when enabled is false/absent so typos never pass silently.
  PieNetworkEmulation::FEmulationSpec EmulationSpec;
  {
    bool bEmulationEnabled = false;
    FString EmulationTargetStr;
    FString EmulationProfileStr;
    if (TSharedPtr<FJsonObject> EmulationObj = Ctx.GetObject(TEXT("networkEmulation"))) {
      EmulationObj->TryGetBoolField(TEXT("enabled"), bEmulationEnabled);
      EmulationObj->TryGetStringField(TEXT("target"), EmulationTargetStr);
      EmulationObj->TryGetStringField(TEXT("profile"), EmulationProfileStr);
    }
    FString EmulationError;
    if (!PieNetworkEmulation::BuildEmulationSpec(bEmulationEnabled, EmulationTargetStr,
            EmulationProfileStr, PieControlUtils::GetAvailableEmulationProfiles(),
            EmulationSpec, EmulationError)) {
      Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, EmulationError);
      return true;
    }
  }

  const TSharedPtr<PinWrightPieState::FPieLifecycleOperation> OperationPtr =
      GPieLifecycleOperationOwner.TryBegin(
          PinWrightPieState::EPieLifecycleOperationType::Play);
  if (!OperationPtr.IsValid()) {
    Ctx.SendError(ErrorCodes::ERR_PIE_START_IN_PROGRESS,
        TEXT("A PIE lifecycle operation started before this play request could take ownership"));
    return true;
  }
  const TSharedRef<PinWrightPieState::FPieLifecycleOperation> Operation =
      OperationPtr.ToSharedRef();

  if (!PieControlUtils::RequestPlayInEditorSession(NumClients, NetMode, EmulationSpec)) {
    GPieLifecycleOperationOwner.Complete(Operation);
    Ctx.SendError(ErrorCodes::ERR_UNSUPPORTED_ENGINE_VERSION,
        TEXT("numClients/netMode/networkEmulation overrides need ULevelEditorPlaySettings, which is unavailable on this engine"));
    return true;
  }

  TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
  Resp->SetBoolField(TEXT("requested"), true);
  const auto AddAppliedSessionSettings =
      [NumClients, NetMode, NetModeStr, EmulationSpec](const TSharedPtr<FJsonObject>& Response)
  {
    if (NumClients > 0) {
      Response->SetNumberField(TEXT("numClients"), NumClients);
    }
    if (NetMode >= 0) {
      Response->SetStringField(TEXT("netMode"), NetModeStr.ToLower());
    }
    // These fields describe a session only after PlayWorld proves that one started.
    TSharedPtr<FJsonObject> EmulationResp = MakeShared<FJsonObject>();
    EmulationResp->SetBoolField(TEXT("enabled"), EmulationSpec.bEnabled);
    if (EmulationSpec.bEnabled) {
      EmulationResp->SetStringField(TEXT("target"), PieNetworkEmulation::TargetToString(EmulationSpec.Target));
      EmulationResp->SetStringField(TEXT("profile"), EmulationSpec.Profile);
    }
    Response->SetObjectField(TEXT("networkEmulation"), EmulationResp);
  };

  const PinWrightPieState::FPieLifecycleState InitialState =
      PinWrightPieState::CaptureEditorPieLifecycleState();
  if (InitialState.bPieActive) {
    AddAppliedSessionSettings(Resp);
    Resp->SetBoolField(TEXT("success"), true);
    Resp->SetBoolField(TEXT("pieActive"), true);
    Resp->SetBoolField(TEXT("sessionInProgress"), InitialState.bSessionInProgress);
    Resp->SetBoolField(TEXT("startRequestQueued"), InitialState.bStartRequestQueued);
    Resp->SetBoolField(TEXT("timedOut"), false);
    Resp->SetBoolField(TEXT("cancelled"), false);
    Resp->SetStringField(TEXT("message"), TEXT("PIE session started"));
    GPieLifecycleOperationOwner.Complete(Operation);
    Ctx.SendSuccess(Resp);
    return true;
  }

  const TSharedRef<FAsyncResponseToken> Token = Ctx.MakeAsyncToken();
  const TSharedRef<FPieDispatcherWaitState> WaitState =
      MakeShared<FPieDispatcherWaitState>();
  const TSharedPtr<FAsyncRequestLifetimeLease> LifetimeLease =
      Ctx.RetainAsyncRequestLifetime(
          [Operation, WaitState, Token, Resp]()
          {
            AbandonPieLifecycleWait(
                Operation,
                WaitState,
                Token,
                Resp,
                ErrorCodes::ERR_PIE_START_FAILED,
                TEXT("Dispatcher ended while waiting for PIE to create PlayWorld"));
          });

  WaitState->WaitControl = PinWrightPieState::WaitForPieLifecycleState(
      PinWrightPieState::EPieLifecycleWaitTarget::PlayWorldActive,
      GPieLifecycleTimeoutSeconds,
      [Token, Resp, AddAppliedSessionSettings, Operation, WaitState, LifetimeLease](
          const PinWrightPieState::FPieLifecycleWaitResult& Outcome)
      {
        Resp->SetBoolField(TEXT("pieActive"), Outcome.bPieActive);
        Resp->SetBoolField(TEXT("sessionInProgress"), Outcome.bSessionInProgress);
        Resp->SetBoolField(TEXT("startRequestQueued"), Outcome.bStartRequestQueued);
        Resp->SetBoolField(TEXT("timedOut"), Outcome.bTimedOut);
        Resp->SetBoolField(TEXT("cancelled"), Outcome.bCancelled);
        if (Outcome.bCancelled)
        {
          WaitState->bResponseSent = true;
          ReleaseDispatcherLifetime(LifetimeLease);
          Resp->SetBoolField(TEXT("success"), false);
          GPieLifecycleOperationOwner.Complete(Operation);
          Token->SendError(ErrorCodes::ERR_PIE_START_CANCELLED,
              TEXT("editor.stop invalidated this PIE start request generation"),
              Resp);
          return;
        }
        if (Outcome.bTimedOut)
        {
          WaitState->bResponseSent = true;
          Resp->SetBoolField(TEXT("success"), false);
          const PinWrightPieState::FPieLifecycleState TimeoutState = {
              Outcome.bPieActive,
              Outcome.bSessionInProgress,
              Outcome.bStartRequestQueued,
          };
          const TSharedRef<FPieStopActions> CleanupActions = MakeShared<FPieStopActions>();
          ApplyPieStopAction(TimeoutState, *CleanupActions);
          PinWrightPieState::FPieLifecycleState CurrentState =
              PinWrightPieState::CaptureEditorPieLifecycleState();
          if (CurrentState.bPieActive || CurrentState.bSessionInProgress ||
              CurrentState.bStartRequestQueued)
          {
            // The engine already consumed the queued request when only session info remains.
            // Keep this generation owned while a bounded, token-free watcher waits for a late
            // PlayWorld and requests end play. Never invalidate engine session info here.
            StartPieTimeoutCleanupWatcher(
                Operation, CleanupActions, WaitState, LifetimeLease);
          }
          else
          {
            GPieLifecycleOperationOwner.Complete(Operation);
            ReleaseDispatcherLifetime(LifetimeLease);
          }
          CurrentState = PinWrightPieState::CaptureEditorPieLifecycleState();
          Resp->SetBoolField(TEXT("pieActive"), CurrentState.bPieActive);
          Resp->SetBoolField(TEXT("sessionInProgress"), CurrentState.bSessionInProgress);
          Resp->SetBoolField(TEXT("startRequestQueued"), CurrentState.bStartRequestQueued);
          Resp->SetBoolField(TEXT("pieActiveAtTimeout"), Outcome.bPieActive);
          Resp->SetBoolField(TEXT("sessionInProgressAtTimeout"), Outcome.bSessionInProgress);
          Resp->SetBoolField(TEXT("startRequestQueuedAtTimeout"),
              Outcome.bStartRequestQueued);
          AddPieStopActionFields(Resp, *CleanupActions);
          Token->SendError(ErrorCodes::ERR_PIE_START_FAILED,
              TEXT("PIE did not create PlayWorld within 15 seconds"),
              Resp);
          return;
        }
        AddAppliedSessionSettings(Resp);
        WaitState->bResponseSent = true;
        ReleaseDispatcherLifetime(LifetimeLease);
        Resp->SetBoolField(TEXT("success"), true);
        Resp->SetStringField(TEXT("message"), TEXT("PIE session started"));
        GPieLifecycleOperationOwner.Complete(Operation);
        Token->SendSuccess(Resp);
      },
      PinWrightPieState::CaptureEditorPieLifecycleState,
      [Operation]()
      {
        return !GPieLifecycleOperationOwner.IsCurrent(Operation);
      });
  return true;
}

// ---- editor.stop ----
REGISTER_RPC_HANDLER("editor.stop", "editor", "Stop an active or pending Play-In-Editor session and answer only after Unreal reports that no PIE session remains (or a bounded teardown timeout). A live PlayWorld receives an end request; a queued start is cancelled, while a consumed request with session info but no PlayWorld is watched until it can be ended safely. Idempotent: returns alreadyStopped=true if no PIE session is running.",
    RPC_NO_PARAMS)
{
  if (!GEditor) {
    Ctx.SendError(ErrorCodes::ERR_EDITOR_NOT_AVAILABLE, TEXT("Editor not available"));
    return true;
  }

  const PinWrightPieState::FPieLifecycleState InitialState =
      PinWrightPieState::CaptureEditorPieLifecycleState();

  bool bCancelledPlayWaiter = false;
  if (const TSharedPtr<PinWrightPieState::FPieLifecycleOperation> ActiveOperation =
          GPieLifecycleOperationOwner.GetActiveOperation())
  {
    if (ActiveOperation->Type != PinWrightPieState::EPieLifecycleOperationType::Play)
    {
      TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
      Resp->SetBoolField(TEXT("success"), false);
      Resp->SetBoolField(TEXT("pieActive"), InitialState.bPieActive);
      Resp->SetBoolField(TEXT("sessionInProgress"), InitialState.bSessionInProgress);
      Resp->SetBoolField(TEXT("startRequestQueued"), InitialState.bStartRequestQueued);
      Resp->SetBoolField(TEXT("timedOut"), false);
      Resp->SetStringField(TEXT("activeLifecycleOperation"),
          PieLifecycleOperationName(ActiveOperation->Type));
      Ctx.SendError(ErrorCodes::ERR_PIE_STOP_IN_PROGRESS,
          TEXT("A PIE teardown or timeout-cleanup operation is already waiting for the session to become inactive"),
          Resp);
      return true;
    }

    // Snapshot above came first: cancelling the software waiter must not erase the active-world
    // fact used below. The waiter's cancellation check runs before its target check.
    GPieLifecycleOperationOwner.CancelActiveOperation();
    bCancelledPlayWaiter = true;
  }

  if (!InitialState.bPieActive && !InitialState.bSessionInProgress) {
    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetBoolField(TEXT("success"), true);
    Resp->SetBoolField(TEXT("alreadyStopped"), true);
    Resp->SetBoolField(TEXT("pieActive"), InitialState.bPieActive);
    Resp->SetBoolField(TEXT("sessionInProgress"), false);
    Resp->SetBoolField(TEXT("startRequestQueued"), InitialState.bStartRequestQueued);
    Resp->SetBoolField(TEXT("timedOut"), false);
    Resp->SetBoolField(TEXT("cancelled"), false);
    Resp->SetBoolField(TEXT("cancelledPlayWaiter"), bCancelledPlayWaiter);
    Ctx.SendSuccess(Resp);
    return true;
  }

  const TSharedPtr<PinWrightPieState::FPieLifecycleOperation> OperationPtr =
      GPieLifecycleOperationOwner.TryBegin(
          PinWrightPieState::EPieLifecycleOperationType::Stop);
  if (!OperationPtr.IsValid()) {
    Ctx.SendError(ErrorCodes::ERR_PIE_STOP_IN_PROGRESS,
        TEXT("A PIE lifecycle operation started before this stop request could take ownership"));
    return true;
  }
  const TSharedRef<PinWrightPieState::FPieLifecycleOperation> Operation =
      OperationPtr.ToSharedRef();

  const TSharedRef<FPieStopActions> Actions = MakeShared<FPieStopActions>();

  TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
  Resp->SetBoolField(TEXT("requested"), true);
  Resp->SetBoolField(TEXT("cancelledPlayWaiter"), bCancelledPlayWaiter);
  AddPieStopActionFields(Resp, *Actions);

  // A consumed start can expose session info before PlayWorld. Keep probing without cancelling
  // that state; when PlayWorld appears, the same probe requests end play. During normal teardown,
  // it also re-issues a lost end request and waits for authoritative session inactivity.
  const auto CaptureStopState = [Actions, Operation]()
  {
    return CaptureAndApplyPieStopState(Operation, *Actions);
  };

  const PinWrightPieState::FPieLifecycleState StateAfterRequest = CaptureStopState();
  if (PinWrightPieState::EvaluatePieLifecycleWait(
          PinWrightPieState::EPieLifecycleWaitTarget::SessionInactive,
          StateAfterRequest,
          false,
          false) == PinWrightPieState::EPieLifecycleWaitStatus::Reached)
  {
    Resp->SetBoolField(TEXT("success"), true);
    Resp->SetBoolField(TEXT("pieActive"), StateAfterRequest.bPieActive);
    Resp->SetBoolField(TEXT("sessionInProgress"), false);
    Resp->SetBoolField(TEXT("startRequestQueued"), StateAfterRequest.bStartRequestQueued);
    Resp->SetBoolField(TEXT("timedOut"), false);
    Resp->SetBoolField(TEXT("cancelled"), false);
    AddPieStopActionFields(Resp, *Actions);
    Resp->SetStringField(TEXT("message"), TEXT("PIE session stopped"));
    GPieLifecycleOperationOwner.Complete(Operation);
    Ctx.SendSuccess(Resp);
    return true;
  }

  const TSharedRef<FAsyncResponseToken> Token = Ctx.MakeAsyncToken();
  const TSharedRef<FPieDispatcherWaitState> WaitState =
      MakeShared<FPieDispatcherWaitState>();
  const TSharedPtr<FAsyncRequestLifetimeLease> LifetimeLease =
      Ctx.RetainAsyncRequestLifetime(
          [Operation, WaitState, Token, Resp, Actions]()
          {
            AbandonPieLifecycleWait(
                Operation,
                WaitState,
                Token,
                Resp,
                ErrorCodes::ERR_PIE_STOP_FAILED,
                TEXT("Dispatcher ended while waiting for PIE to become inactive"),
                Actions);
          });

  WaitState->WaitControl = PinWrightPieState::WaitForPieLifecycleState(
      PinWrightPieState::EPieLifecycleWaitTarget::SessionInactive,
      GPieLifecycleTimeoutSeconds,
      [Token, Resp, Actions, Operation, WaitState, LifetimeLease](
          const PinWrightPieState::FPieLifecycleWaitResult& Outcome)
      {
        Resp->SetBoolField(TEXT("pieActive"), Outcome.bPieActive);
        Resp->SetBoolField(TEXT("sessionInProgress"), Outcome.bSessionInProgress);
        Resp->SetBoolField(TEXT("startRequestQueued"), Outcome.bStartRequestQueued);
        Resp->SetBoolField(TEXT("timedOut"), Outcome.bTimedOut);
        Resp->SetBoolField(TEXT("cancelled"), Outcome.bCancelled);
        if (Outcome.bTimedOut)
        {
          WaitState->bResponseSent = true;
          ReleaseDispatcherLifetime(LifetimeLease);
          Resp->SetBoolField(TEXT("success"), false);
          const PinWrightPieState::FPieLifecycleState TimeoutState = {
              Outcome.bPieActive,
              Outcome.bSessionInProgress,
              Outcome.bStartRequestQueued,
          };
          const PinWrightPieState::FPieLifecycleState CurrentState =
              CaptureAndApplyPieStopState(Operation, *Actions);
          Resp->SetBoolField(TEXT("pieActive"), CurrentState.bPieActive);
          Resp->SetBoolField(TEXT("sessionInProgress"), CurrentState.bSessionInProgress);
          Resp->SetBoolField(TEXT("startRequestQueued"), CurrentState.bStartRequestQueued);
          Resp->SetBoolField(TEXT("pieActiveAtTimeout"), TimeoutState.bPieActive);
          Resp->SetBoolField(TEXT("sessionInProgressAtTimeout"),
              TimeoutState.bSessionInProgress);
          Resp->SetBoolField(TEXT("startRequestQueuedAtTimeout"),
              TimeoutState.bStartRequestQueued);
          AddPieStopActionFields(Resp, *Actions);
          GPieLifecycleOperationOwner.Complete(Operation);
          Token->SendError(ErrorCodes::ERR_PIE_STOP_FAILED,
              TEXT("PIE did not become inactive within 15 seconds"),
              Resp);
          return;
        }
        WaitState->bResponseSent = true;
        ReleaseDispatcherLifetime(LifetimeLease);
        Resp->SetBoolField(TEXT("success"), true);
        AddPieStopActionFields(Resp, *Actions);
        Resp->SetStringField(TEXT("message"), TEXT("PIE session stopped"));
        GPieLifecycleOperationOwner.Complete(Operation);
        Token->SendSuccess(Resp);
      },
      CaptureStopState);
  return true;
}

// ---- editor.pause ----
REGISTER_RPC_HANDLER("editor.pause", "editor", "Pause the active Play-In-Editor session: the world stops ticking AND the session's UI stops advancing, so the frame you capture next is the frame that was on screen. Freezing the UI is two separate levers - the UMG animation clock (Slate's fixed-delta tick, driven to 0) and the Slate tick flag of every UUserWidget owned by a PIE world (switched off, so a HUD that animates from its own Tick stops instead of running on against a frozen world). A freeze also holds r.MotionBlurQuality at 0 (reported as motionBlurSuppressed), because a world that is not ticking still renders velocity vectors describing motion from before the pause, which smears an otherwise correct capture. All of it is restored by editor.resume, editor.stop, and by the editor's own Resume/Stop buttons. Pass freezeUi:false for the pre-2026-09 world-only pause. Editor-owned widgets (utility widgets, asset editors) are never touched. Errors with NO_ACTIVE_SESSION if PIE is not running.",
    RPC_PARAMS(
        RPC_PARAM_OPT("freezeUi", "boolean", "Freeze the session's UI clocks along with the world (default true). false pauses the world only - UMG animations and widget Tick keep running on real time, which is what made a short-lived HUD animation uncapturable.")
    ))
{
  if (!GEditor) {
    Ctx.SendError(ErrorCodes::ERR_EDITOR_NOT_AVAILABLE, TEXT("Editor not available"));
    return true;
  }
  if (!GEditor->PlayWorld) {
    Ctx.SendError(ErrorCodes::ERR_NO_ACTIVE_SESSION, TEXT("No active PIE session to pause"));
    return true;
  }

  GEditor->PlayWorld->bDebugPauseExecution = true;

  const bool bFreezeUi = Ctx.GetBool(TEXT("freezeUi"), true);
  if (bFreezeUi) {
    PinWrightPieTime::Freeze();
  } else {
    // Explicit opt-out also UNDOES a freeze an earlier pause/step left in place, so the
    // parameter always describes the state the session ends up in.
    PinWrightPieTime::Release();
  }

  TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
  Resp->SetBoolField(TEXT("success"), true);
  Resp->SetStringField(TEXT("state"), TEXT("paused"));
  Resp->SetBoolField(TEXT("uiFrozen"), PinWrightPieTime::IsFrozen());
  Resp->SetBoolField(TEXT("motionBlurSuppressed"), PinWrightPieTime::IsMotionBlurSuppressed());
  Resp->SetStringField(TEXT("message"), bFreezeUi
      ? TEXT("PIE session paused; world and UI clocks frozen")
      : TEXT("PIE session paused; UI left running on real time (freezeUi:false)"));
  Ctx.SendSuccess(Resp);
  return true;
}

// ---- editor.resume ----
REGISTER_RPC_HANDLER("editor.resume", "editor", "Resume a paused Play-In-Editor session, restoring the world tick and everything editor.pause or editor.step_frame held (the Slate fixed-delta cvar and its delta, the process fixed time step, the stepped PIE world context's own fixed tick, r.MotionBlurQuality, and the widget tick flags, which are recomputed rather than replayed from a saved value). Errors with NO_ACTIVE_SESSION if PIE is not running.",
    RPC_NO_PARAMS)
{
  if (!GEditor) {
    Ctx.SendError(ErrorCodes::ERR_EDITOR_NOT_AVAILABLE, TEXT("Editor not available"));
    return true;
  }
  if (!GEditor->PlayWorld) {
    Ctx.SendError(ErrorCodes::ERR_NO_ACTIVE_SESSION, TEXT("No active PIE session to resume"));
    return true;
  }

  // Thaw the UI before the world: the first resumed frame must tick the widgets it ticks.
  const bool bWasFrozen = PinWrightPieTime::IsFrozen();
  PinWrightPieTime::Release();

  GEditor->PlayWorld->bDebugPauseExecution = false;

  TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
  Resp->SetBoolField(TEXT("success"), true);
  Resp->SetStringField(TEXT("state"), TEXT("resumed"));
  Resp->SetBoolField(TEXT("uiThawed"), bWasFrozen);
  Resp->SetStringField(TEXT("message"), TEXT("PIE session resumed"));
  Ctx.SendSuccess(Resp);
  return true;
}

// ---- editor.step_frame ----
REGISTER_RPC_HANDLER("editor.step_frame", "editor", "Advance the paused PIE session by exactly one frame of deltaSeconds and freeze again, world and UI together. The world tick, the Niagara/FX tick it drives and the UMG animation tick all run at the requested delta (the stepped PIE world context's own fixed tick, which is what actually sizes an editor PIE tick, plus the process fixed time step and the Slate fixed delta - all engaged only for this frame); widget Tick, whose delta the engine does not expose, is switched back on for the stepped frame and off again after. Answers AFTER the frame has run and reports what it actually bought: worldSecondsAdvanced (from UWorld::TimeSeconds), uiSecondsAdvanced, and deltaHonoured - false when the level's own AWorldSettings clamp or a time dilation stopped the world advancing the full request, with worldSecondsExpected, worldDeltaClamp and timeDilation naming what was in force. Motion blur is held at 0 for the life of the freeze (motionBlurSuppressed), because a world that is not ticking still carries velocity vectors describing motion from before the pause. This is the way to capture a sub-second HUD animation: pause, trigger, step, screenshot. Errors NO_ACTIVE_SESSION if PIE is not running, STEP_IN_PROGRESS if a previous step has not answered yet, INVALID_ARGUMENT on an out-of-range deltaSeconds.",
    RPC_PARAMS(
        RPC_PARAM_OPT("deltaSeconds", "number", "Length of the step in SECONDS (default 1/60). Must be > 0 and <= 1.0; a larger value is refused as a units mistake. A single world tick is still bounded by the level's AWorldSettings (MinUndilatedFrameTime / MaxUndilatedFrameTime) and scaled by time dilation, so a step can buy less - or more - world time than requested; when it does, deltaHonoured is false and worldDeltaClamp names which bound bit.")
    ))
{
  if (!GEditor) {
    Ctx.SendError(ErrorCodes::ERR_EDITOR_NOT_AVAILABLE, TEXT("Editor not available"));
    return true;
  }
  if (!GEditor->PlayWorld) {
    Ctx.SendError(ErrorCodes::ERR_NO_ACTIVE_SESSION, TEXT("No active PIE session to step"));
    return true;
  }

  double DeltaSeconds = 0.0;
  FString ValidationReason;
  if (!PinWrightPieTime::ValidateStepDelta(Ctx.GetNumber(TEXT("deltaSeconds"), 0.0), DeltaSeconds, ValidationReason)) {
    Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, ValidationReason);
    return true;
  }

  // Refuse through Ctx, not through a token: a synchronous answer must keep the FResponseCapture
  // path (text formatters, test fixtures), which MakeAsyncToken deliberately drops.
  FString ErrorCode;
  FString Reason;
  if (!PinWrightPieTime::CanStep(ErrorCode, Reason)) {
    Ctx.SendError(ErrorCode, Reason);
    return true;
  }

  // The success response has to outlive this handler invocation: the frame it reports on has not
  // run yet (the world ticks, then Slate, then the core ticker this handler is called from).
  TSharedRef<FAsyncResponseToken> Token = Ctx.MakeAsyncToken();

  const bool bStarted = PinWrightPieTime::Step(DeltaSeconds,
      [Token](const PinWrightPieTime::FStepOutcome& Outcome)
      {
        TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
        Resp->SetBoolField(TEXT("success"), true);
        Resp->SetBoolField(TEXT("stepped"), !Outcome.bTimedOut);
        Resp->SetNumberField(TEXT("deltaSeconds"), Outcome.RequestedDeltaSeconds);
        Resp->SetNumberField(TEXT("worldSecondsAdvanced"), Outcome.WorldSecondsAdvanced);
        Resp->SetNumberField(TEXT("uiSecondsAdvanced"), Outcome.UiSecondsAdvanced);
        Resp->SetBoolField(TEXT("deltaHonoured"), Outcome.bDeltaHonoured);
        Resp->SetNumberField(TEXT("worldSecondsExpected"), Outcome.Budget.ExpectedSeconds);
        Resp->SetNumberField(TEXT("timeDilation"), Outcome.Budget.TimeDilation);
        Resp->SetStringField(TEXT("worldDeltaClamp"), Outcome.Budget.ClampSource);
        Resp->SetBoolField(TEXT("uiFrozen"), PinWrightPieTime::IsFrozen());
        Resp->SetBoolField(TEXT("motionBlurSuppressed"), PinWrightPieTime::IsMotionBlurSuppressed());
        FString Message;
        if (Outcome.bTimedOut) {
          Message = TEXT("Step timed out waiting for the frame; the session was re-paused and the clocks restored");
        } else if (Outcome.bDeltaHonoured) {
          Message = TEXT("Stepped one frame; world and UI re-frozen");
        } else {
          Message = FString::Printf(
              TEXT("Stepped one frame, but the world advanced %.6g s rather than the requested %.6g s; "
                   "this level's world settings allow %.6g s for one tick (%s). World and UI re-frozen."),
              Outcome.WorldSecondsAdvanced, Outcome.RequestedDeltaSeconds,
              Outcome.Budget.ExpectedSeconds, Outcome.Budget.ClampSource);
        }
        Resp->SetStringField(TEXT("message"), Message);
        Token->SendSuccess(Resp);
      },
      ErrorCode, Reason);

  if (!bStarted) {
    Token->SendError(ErrorCode, Reason);
  }
  return true;
}

// ---- editor.eject ----
REGISTER_RPC_HANDLER("editor.eject", "editor", "Eject the player controller from its possessed pawn during PIE so the camera flies free. Uses the editor's F8 possess/eject mechanism (RequestToggleBetweenPIEandSIE) to detach a possessed PIE session to Simulate-In-Editor. No-op with alreadyEjected=true if the session is already simulating. Requires an active PIE session (errors NO_ACTIVE_SESSION). The toggle is queued and applied on the next editor tick, and only in an interactive editor with a Slate viewport.",
    RPC_NO_PARAMS)
{
  if (!GEditor) {
    Ctx.SendError(ErrorCodes::ERR_EDITOR_NOT_AVAILABLE, TEXT("Editor not available"));
    return true;
  }
  if (!GEditor->PlayWorld) {
    Ctx.SendError(ErrorCodes::ERR_NO_ACTIVE_SESSION, TEXT("Cannot eject: Play session not active"));
    return true;
  }

  // There is no "Eject" console command (an earlier version ran a nonexistent exec and
  // reported success unconditionally). The editor's real F8 eject flips a possessed PIE
  // session into Simulate-In-Editor via RequestToggleBetweenPIEandSIE(); when already
  // simulating, the player is already ejected, so requesting the toggle would re-possess.
  TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
  Resp->SetBoolField(TEXT("success"), true);
  if (GEditor->bIsSimulatingInEditor) {
    Resp->SetBoolField(TEXT("ejected"), true);
    Resp->SetBoolField(TEXT("alreadyEjected"), true);
    Resp->SetStringField(TEXT("message"), TEXT("Player already ejected (Simulate-In-Editor)"));
  } else {
    GEditor->RequestToggleBetweenPIEandSIE();
    Resp->SetBoolField(TEXT("ejected"), true);
    Resp->SetBoolField(TEXT("toggleQueued"), true);
    Resp->SetStringField(TEXT("message"), TEXT("Eject requested; detaching to Simulate-In-Editor on the next editor tick"));
  }
  Ctx.SendSuccess(Resp);
  return true;
}

// ---- editor.possess ----
REGISTER_RPC_HANDLER("editor.possess", "editor", "Possess the named actor with the player controller during PIE. Calls the PIE player controller's Possess() directly on the target Pawn, then verifies a player controller actually possesses it and echoes possessedPawnPath plus possessingControllerPath (the controller that took it, matching editor.status's playerControllerPath). Requires an active PIE session (errors NOT_IN_PIE); rejects a non-Pawn target with NOT_A_PAWN; errors NO_PLAYER_CONTROLLER if the PIE world has no player controller; if possession does not transfer control, returns POSSESS_FAILED instead of fake success.",
    RPC_PARAMS(
        ParamAliasUtils::MakeAliasParamSpec(TEXT("actorName"), TEXT("string"),
            TEXT("Label or name of an actor in the active PIE world to possess. Also accepts an objectPath alias."),
            /*bRequired=*/true, TArray<FString>({TEXT("actorName"), TEXT("objectPath")}))
    ))
{
  FString ActorName = Ctx.GetStringFirstOf({TEXT("actorName"), TEXT("objectPath")});

  if (ActorName.IsEmpty()) {
    Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, TEXT("actorName required"));
    return true;
  }

  AActor *Found = McpActorUtils::FindActorByName(nullptr, ActorName);
  if (!Found) {
    Ctx.SendError(ErrorCodes::ERR_ACTOR_NOT_FOUND,
        FString::Printf(TEXT("Actor not found: %s"), *ActorName));
    return true;
  }

  if (GEditor) {
    // APlayerController::Possess only accepts a Pawn; a non-Pawn (SkeletalMeshActor,
    // StaticMeshActor, a light, ...) can never be possessed regardless of PIE state, so
    // reject it up front with the actor's class — the caller is never told a
    // possession "worked" when nothing happened. Resolve the typed Pawn once here so the
    // Possess call and the verification loop below reuse it instead of re-walking the
    // class hierarchy.
    APawn* TargetPawn = Cast<APawn>(Found);
    if (!TargetPawn) {
      Ctx.SendError(ErrorCodes::ERR_NOT_A_PAWN,
          FString::Printf(TEXT("Cannot possess '%s': it is a %s, not a Pawn"),
              *ActorName, *Found->GetClass()->GetName()));
      return true;
    }

    if (!GEditor->PlayWorld) {
      Ctx.SendError(ErrorCodes::ERR_NOT_IN_PIE, TEXT("Cannot possess actor while not in PIE"));
      return true;
    }

    // There is no "POSSESS" console command (an earlier version ran a nonexistent exec, so
    // it only ever "succeeded" when the pawn was already possessed). Drive the real API:
    // resolve the PIE player controller and Possess the target Pawn directly.
    APlayerController* PlayerController = GEditor->PlayWorld->GetFirstPlayerController();
    if (!PlayerController) {
      Ctx.SendError(ErrorCodes::ERR_NO_PLAYER_CONTROLLER,
          TEXT("No player controller in the active PIE world to possess with"));
      return true;
    }
    PlayerController->Possess(TargetPawn);

    // Verify the takeover actually landed: walk the PIE player controllers and confirm
    // one now possesses the requested Pawn. Possess can be rejected (auto-possession by
    // another controller, authority rules), so read back the controller's resulting Pawn.
    // Capture WHICH controller took it so the result self-correlates with
    // editor.status's playerControllerPath (instead of only echoing the pawn the
    // caller already named).
    FString PossessedPawnPath;
    FString PossessingControllerPath;
    for (FConstPlayerControllerIterator It = GEditor->PlayWorld->GetPlayerControllerIterator(); It; ++It) {
      if (APlayerController* PC = It->Get()) {
        if (PC->GetPawn() == TargetPawn) {
          PossessedPawnPath = TargetPawn->GetPathName();
          PossessingControllerPath = PC->GetPathName();
          break;
        }
      }
    }

    if (PossessedPawnPath.IsEmpty()) {
      Ctx.SendError(ErrorCodes::ERR_POSSESS_FAILED,
          FString::Printf(TEXT("Possess did not transfer control to '%s' (%s); no player controller possesses it afterward"),
              *ActorName, *Found->GetClass()->GetName()));
      return true;
    }

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetBoolField(TEXT("success"), true);
    Resp->SetStringField(TEXT("possessedPawnPath"), PossessedPawnPath);
    Resp->SetStringField(TEXT("possessingControllerPath"), PossessingControllerPath);
    Ctx.SendSuccess(Resp);
    return true;
  }

  Ctx.SendError(ErrorCodes::ERR_EDITOR_NOT_AVAILABLE, TEXT("Editor not available"));
  return true;
}

// ---- editor.status ----
REGISTER_RPC_HANDLER("editor.status", "editor",
    "Read-only probe for PIE / editor state. Returns inPie, pieIsPaused, uiFrozen, pieWorldPath, editorWorldPath, playerControllerPath, elapsedPieSeconds. uiFrozen tells the two apart: pieIsPaused is the world tick, uiFrozen is whether editor.pause / editor.step_frame also has the session's UMG animation clock and widget ticks held (see editor.pause). No side effects — safe pre-flight check.",
    RPC_NO_PARAMS)
{
    if (!GEditor) {
        Ctx.SendError(ErrorCodes::ERR_EDITOR_NOT_AVAILABLE, TEXT("Editor not available"));
        return true;
    }

    UWorld* PieWorld = GEditor->PlayWorld;
    if (!PieWorld) {
        for (const FWorldContext& WCtx : GEditor->GetWorldContexts()) {
            if (WCtx.WorldType == EWorldType::PIE && WCtx.World()) {
                PieWorld = WCtx.World();
                break;
            }
        }
    }
    UWorld* EditorWorld = GEditor->GetEditorWorldContext().World();

    const bool bInPie = (PieWorld != nullptr);
    bool bPieIsPaused = false;
    if (PieWorld) {
        bPieIsPaused = PieWorld->bDebugPauseExecution || GEditor->bIsSimulatingInEditor;
    }

    FString PieWorldPath = PieWorld ? PieWorld->GetPathName() : FString();
    FString EditorWorldPath = EditorWorld ? EditorWorld->GetPathName() : FString();

    FString PlayerControllerPath;
    if (PieWorld) {
        for (FConstPlayerControllerIterator It = PieWorld->GetPlayerControllerIterator(); It; ++It) {
            if (APlayerController* PC = It->Get()) {
                PlayerControllerPath = PC->GetPathName();
                break;
            }
        }
    }

    const double ElapsedPieSeconds = PieWorld ? PieWorld->TimeSeconds : 0.0;

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetBoolField(TEXT("inPie"), bInPie);
    Resp->SetBoolField(TEXT("pieIsPaused"), bPieIsPaused);
    Resp->SetBoolField(TEXT("uiFrozen"), PinWrightPieTime::IsFrozen());
    Resp->SetStringField(TEXT("pieWorldPath"), PieWorldPath);
    Resp->SetStringField(TEXT("editorWorldPath"), EditorWorldPath);
    Resp->SetStringField(TEXT("playerControllerPath"), PlayerControllerPath);
    Resp->SetNumberField(TEXT("elapsedPieSeconds"), ElapsedPieSeconds);
    Ctx.SendSuccess(Resp);
    return true;
}

// ---- editor.pie_status ----
REGISTER_RPC_HANDLER("editor.pie_status", "editor",
    "Read-only probe enumerating every live PIE world context (multi-instance aware, unlike editor.status which surfaces only the first PIE world). Returns inPie, count, and contexts[] with {pieInstance, kind: server|client|standalone, netMode, mapName, worldPath, gameStateClass, numPlayerControllers}; contexts is empty when PIE is not running. While a session is active it also returns top-level networkEmulation {enabled, target?, profile?} — the PIE network-emulation state actually in effect for the running session (read from the engine's session settings copy, so it is truthful for toolbar-started sessions too), letting measured sessions record their wire state. Top-level uiFrozen reports whether editor.pause / editor.step_frame currently holds the session's UI clocks (it is session-wide, not per-context). Use it to pick a 'world' selector for editor.console_command in multiplayer-in-PIE sessions. No side effects — walks only the engine world contexts and their direct members.",
    RPC_NO_PARAMS)
{
    if (!GEditor) {
        Ctx.SendError(ErrorCodes::ERR_EDITOR_NOT_AVAILABLE, TEXT("Editor not available"));
        return true;
    }

    const TArray<PieWorldSelector::FPieContextInfo> PieContexts = PieWorldSelector::GatherPieContexts();

    TArray<TSharedPtr<FJsonValue>> ContextsArr;
    ContextsArr.Reserve(PieContexts.Num());
    for (const PieWorldSelector::FPieContextInfo& Info : PieContexts)
    {
        // GatherPieContexts only returns entries with a resolved, non-null world.
        UWorld* World = Info.World;
        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetNumberField(TEXT("pieInstance"), Info.PieInstance);
        Entry->SetStringField(TEXT("kind"), PieWorldSelector::ClassifyNetMode(Info.NetMode));
        Entry->SetStringField(TEXT("netMode"), PieWorldSelector::NetModeToString(Info.NetMode));
        Entry->SetStringField(TEXT("mapName"), UWorld::RemovePIEPrefix(World->GetMapName()));
        Entry->SetStringField(TEXT("worldPath"), World->GetPathName());

        // Empty until a GameState exists — on clients it can lag the server while connecting.
        AGameStateBase* GameState = World->GetGameState();
        Entry->SetStringField(TEXT("gameStateClass"),
            GameState ? GameState->GetClass()->GetName() : FString());

        int32 NumPlayerControllers = 0;
        for (FConstPlayerControllerIterator It = World->GetPlayerControllerIterator(); It; ++It)
        {
            if (It->Get())
            {
                ++NumPlayerControllers;
            }
        }
        Entry->SetNumberField(TEXT("numPlayerControllers"), NumPlayerControllers);

        ContextsArr.Add(MakeShared<FJsonValueObject>(Entry));
    }

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetBoolField(TEXT("inPie"), ContextsArr.Num() > 0);
    Resp->SetNumberField(TEXT("count"), ContextsArr.Num());
    Resp->SetArrayField(TEXT("contexts"), ContextsArr);
    // Session-wide, not per-context: the freeze holds the Slate clock (process-global) and the
    // widget ticks of every PIE world at once.
    Resp->SetBoolField(TEXT("uiFrozen"), PinWrightPieTime::IsFrozen());

    // Wire state of the running session, read from the engine's session-scoped
    // settings copy (the object every PIE instance login actually consumed) — so
    // measured sessions can record whether/what emulation shaped their traffic.
    // Absent when no session is active. target/profile only when enabled.
    PieNetworkEmulation::FEmulationSpec ActiveEmulation;
    if (PieControlUtils::TryGetActiveSessionEmulation(ActiveEmulation))
    {
        TSharedPtr<FJsonObject> EmulationObj = MakeShared<FJsonObject>();
        EmulationObj->SetBoolField(TEXT("enabled"), ActiveEmulation.bEnabled);
        if (ActiveEmulation.bEnabled)
        {
            EmulationObj->SetStringField(TEXT("target"), PieNetworkEmulation::TargetToString(ActiveEmulation.Target));
            EmulationObj->SetStringField(TEXT("profile"), ActiveEmulation.Profile);
        }
        Resp->SetObjectField(TEXT("networkEmulation"), EmulationObj);
    }
    Ctx.SendSuccess(Resp);
    return true;
}
