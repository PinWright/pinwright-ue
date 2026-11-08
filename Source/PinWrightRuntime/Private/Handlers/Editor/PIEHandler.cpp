// Copyright (c) 2026 Alexander Penkin. MIT License.

// editor.pause, editor.resume, editor.step_frame, editor.status, editor.pie_status: the game-safe
// half of the editor module's PIEHandler.cpp, which keeps play/stop/eject/possess. In a -game
// process the game world takes the PIE session's role (PinWrightPieTime::GetSessionWorld).

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ErrorCodes.h"
#include "Handlers/ParamSpec.h"
#include "Handlers/Drive/DriveLiveResolver.h"
#include "Handlers/Editor/PieNetworkEmulation.h"
#include "Handlers/Editor/PieWorldSelector.h"
#include "Dispatch/PinWrightWorld.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/GameStateBase.h"
#include "GameFramework/PlayerController.h"

#if WITH_EDITOR
#include "Editor.h"
#if __has_include("Settings/LevelEditorPlaySettings.h")
#include "Settings/LevelEditorPlaySettings.h"
#define PW_PIE_STATUS_HAS_PLAY_SETTINGS 1
#endif
#endif
#ifndef PW_PIE_STATUS_HAS_PLAY_SETTINGS
#define PW_PIE_STATUS_HAS_PLAY_SETTINGS 0
#endif

// The clock control behind pause / resume / step_frame. bDebugPauseExecution freezes the
// WORLD only; freezing and stepping the UI (UMG animations and widget Tick) needs the two
// separate levers this owns. Every engine citation for why is in its header.
#include "Handlers/Editor/PieTimeControl.h"

namespace
{
// Network-emulation state actually in effect for the running PIE session, read from the
// engine's session-scoped settings copy (PlayInEditorSessionInfo->OriginalRequestParams
// .EditorPlaySettings - the exact object every PIE instance login consumes; see
// PieControlUtils.h in the editor module for the apply side). Because it reads the session
// copy rather than anything this plugin recorded, it reports the truth for sessions started
// from the editor toolbar too. Returns false when no PIE session is active, in a game process,
// or when the settings class is gated out. A toolbar-started session with hand-edited packet
// values reports the details panel's "Custom" pseudo-profile name in OutSpec.Profile.
bool TryGetActiveSessionEmulation(PieNetworkEmulation::FEmulationSpec& OutSpec)
{
#if PW_PIE_STATUS_HAS_PLAY_SETTINGS
    if (!GEditor)
    {
        return false;
    }
    const TOptional<FPlayInEditorSessionInfo> SessionInfo = GEditor->GetPlayInEditorSessionInfo();
    if (!SessionInfo.IsSet() || !SessionInfo->OriginalRequestParams.EditorPlaySettings)
    {
        return false;
    }
    const FLevelEditorPlayNetworkEmulationSettings& EmulationSettings =
        SessionInfo->OriginalRequestParams.EditorPlaySettings->NetworkEmulationSettings;
    OutSpec.bEnabled = EmulationSettings.bIsNetworkEmulationEnabled;
    switch (EmulationSettings.EmulationTarget)
    {
    case NetworkEmulationTarget::Client:
        OutSpec.Target = PieNetworkEmulation::EEmulationTarget::ClientsOnly;
        break;
    case NetworkEmulationTarget::Any:
        OutSpec.Target = PieNetworkEmulation::EEmulationTarget::Everyone;
        break;
    default:
        OutSpec.Target = PieNetworkEmulation::EEmulationTarget::ServerOnly;
        break;
    }
    OutSpec.Profile = EmulationSettings.CurrentProfile;
    return true;
#else
    (void)OutSpec;
    return false;
#endif
}
}

// ---- editor.pause ----
REGISTER_RPC_HANDLER("editor.pause", "editor", "Pause the active Play-In-Editor session: the world stops ticking AND the session's UI stops advancing, so the frame you capture next is the frame that was on screen. Freezing the UI is two separate levers - the UMG animation clock (Slate's fixed-delta tick, driven to 0) and the Slate tick flag of every UUserWidget owned by a PIE world (switched off, so a HUD that animates from its own Tick stops instead of running on against a frozen world). A freeze also holds r.MotionBlurQuality at 0 (reported as motionBlurSuppressed), because a world that is not ticking still renders velocity vectors describing motion from before the pause, which smears an otherwise correct capture. All of it is restored by editor.resume, editor.stop, and by the editor's own Resume/Stop buttons. Pass freezeUi:false for the pre-2026-09 world-only pause. Editor-owned widgets (utility widgets, asset editors) are never touched. Errors with NO_ACTIVE_SESSION if PIE is not running.",
    RPC_PARAMS(
        RPC_PARAM_OPT("freezeUi", "boolean", "Freeze the session's UI clocks along with the world (default true). false pauses the world only - UMG animations and widget Tick keep running on real time, which is what made a short-lived HUD animation uncapturable.")
    ))
{
  UWorld* const SessionWorld = PinWrightPieTime::GetSessionWorld();
  if (!SessionWorld) {
    Ctx.SendError(ErrorCodes::ERR_NO_ACTIVE_SESSION, TEXT("No active PIE session to pause"));
    return true;
  }

  if (!PinWrightPieTime::SetSessionPaused(SessionWorld, true)) {
    Ctx.SendError(ErrorCodes::ERR_OPERATION_FAILED,
        TEXT("The game mode refused to pause the game world (net client, no local player controller, or pausing not allowed)"));
    return true;
  }

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
  UWorld* const SessionWorld = PinWrightPieTime::GetSessionWorld();
  if (!SessionWorld) {
    Ctx.SendError(ErrorCodes::ERR_NO_ACTIVE_SESSION, TEXT("No active PIE session to resume"));
    return true;
  }

  // Thaw the UI before the world: the first resumed frame must tick the widgets it ticks.
  const bool bWasFrozen = PinWrightPieTime::IsFrozen();
  PinWrightPieTime::Release();

  PinWrightPieTime::SetSessionPaused(SessionWorld, false);

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
  UWorld* const SessionWorld = PinWrightPieTime::GetSessionWorld();
  if (!SessionWorld) {
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

// ---- editor.status ----
REGISTER_RPC_HANDLER("editor.status", "editor",
    "Read-only probe for PIE / editor state. Returns inPie, pieIsPaused, uiFrozen, pieWorldPath, editorWorldPath, playerControllerPath, elapsedPieSeconds. uiFrozen tells the two apart: pieIsPaused is the world tick, uiFrozen is whether editor.pause / editor.step_frame also has the session's UMG animation clock and widget ticks held (see editor.pause). No side effects — safe pre-flight check.",
    RPC_NO_PARAMS)
{
    // A game process has no editor world; its game world takes the PIE world's place.
    const bool bGameProcess = PinWright::IsGameProcess();
    UWorld* PieWorld = nullptr;
    UWorld* EditorWorld = nullptr;
    bool bSimulating = false;
    if (bGameProcess) {
        PieWorld = PinWright::GetTargetWorld();
    } else {
#if WITH_EDITOR
        if (!GEditor) {
            Ctx.SendError(ErrorCodes::ERR_EDITOR_NOT_AVAILABLE, TEXT("Editor not available"));
            return true;
        }

        PieWorld = GEditor->PlayWorld;
        if (!PieWorld) {
            for (const FWorldContext& WCtx : GEditor->GetWorldContexts()) {
                if (WCtx.WorldType == EWorldType::PIE && WCtx.World()) {
                    PieWorld = WCtx.World();
                    break;
                }
            }
        }
        EditorWorld = GEditor->GetEditorWorldContext().World();
        bSimulating = GEditor->bIsSimulatingInEditor;
#endif
    }

    const bool bInPie = (PieWorld != nullptr);
    bool bPieIsPaused = false;
    if (PieWorld) {
        // bDebugPauseExecution pauses a PIE world only (UWorld::IsPaused); a game world pauses
        // through its game mode.
        bPieIsPaused = bGameProcess ? PieWorld->IsPaused() : (PieWorld->bDebugPauseExecution || bSimulating);
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
    Resp->SetBoolField(TEXT("gameProcess"), bGameProcess);
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
    // A game process reports its one game world as the sole context (pieInstance -1).
    const bool bGameProcess = PinWright::IsGameProcess();
#if WITH_EDITOR
    if (!bGameProcess && !GEditor) {
        Ctx.SendError(ErrorCodes::ERR_EDITOR_NOT_AVAILABLE, TEXT("Editor not available"));
        return true;
    }
#endif

    const TArray<PieWorldSelector::FPieContextInfo> PieContexts = FDriveLiveResolver::GatherGameContexts();

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
    Resp->SetBoolField(TEXT("gameProcess"), bGameProcess);
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
    if (!bGameProcess && TryGetActiveSessionEmulation(ActiveEmulation))
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
