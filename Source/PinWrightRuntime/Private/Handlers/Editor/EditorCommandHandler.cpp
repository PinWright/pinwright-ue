// Copyright (c) 2026 Alexander Penkin. MIT License.

// editor.console_command, editor.simulate_input: the game-safe half of the editor module's
// EditorCommandHandler.cpp, which keeps undo/redo, save, open/close, preferences, recording and
// bookmarks. In a -game process the game world stands in for the single PIE world.

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ParamSpec.h"
#include "Handlers/ErrorCodes.h"
#include "Handlers/ScalabilityConsoleGuard.h"
#include "Handlers/ConsoleCommandGuard.h"
#include "Handlers/Drive/DriveGameInput.h"
#include "Handlers/Drive/DriveInput.h"
#include "Handlers/Drive/DriveLiveResolver.h"
#include "Handlers/Editor/PieWorldSelector.h"
#include "Dispatch/PinWrightWorld.h"
#include "Dispatch/SafePoint.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#include "Containers/Ticker.h"
#include "Engine/Engine.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "Framework/Application/SlateApplication.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "HAL/PlatformTime.h"
#include "InputCoreTypes.h"
#include "Widgets/SWindow.h"

// ---- editor.console_command ----
REGISTER_RPC_HANDLER("editor.console_command", "editor", "Execute a console command in the editor world or, via the optional 'world' selector, inside a specific PIE world ('server', 'client', 'client:N', 'pie:N') — the multiplayer-in-PIE routing path (e.g. run 'servertravel ...' in the PIE server world, 'open <ip>' in a PIE client world). With 'world' omitted the target is the editor world when no PIE world runs, the PIE world when exactly one runs, and a TARGET_AMBIGUOUS refusal when several run; the response names the resolved 'world' and sets worldDefaulted. In a PIE world a line the engine does not consume is retried through that world's first local player, the in-game console route, so PlayerController / CheatManager / game-viewport exec commands (EnableCheats, summon, viewmode) work; the response's 'route' says which path consumed it ('engine' or 'localPlayer'). Distinct from system.console_command which targets the broader process; use this for editor-, viewport-, and PIE-world-scoped commands. See editor.pie_status for what PIE worlds exist. In a -game process the game world takes the sole-PIE-world role (world omitted, 'server' or 'client' per its net mode) and 'editor' is EDITOR_ONLY. Returns EXEC_FAILED when no exec handler and no console variable recognised the line (typo, or a command owned by an unloaded module); a success means the line was consumed, NOT that the command's effect succeeded — read that back with a typed verb. A line that SETS a scalability CVar is REFUSED with SCALABILITY_CVAR_USE_TYPED_VERB — either an 'sg.*' group, or any CVar carrying ECVF_Scalability / ECVF_ScalabilityGroup (r.ViewDistanceScale, r.Streaming.PoolSize, r.ScreenPercentage, r.MaxAnisotropy, ...), because a console set pins it at ECVF_SetByConsole above the ECVF_SetByScalability priority the editor's own Settings > Engine Scalability Settings panel writes at, for the rest of the session. Use performance.set_scalability, or pass force:true to accept the pin. READING such a CVar (its name with no value) is not refused, and neither is the aggregate 'scalability N', which routes through Scalability::SetQualityLevels at the panel's own priority. A line whose first command word is QUIT_EDITOR or CLOSE_SLATE_MAINFRAME is REFUSED with EDITOR_QUIT_USE_TYPED_VERB (use editor.quit), PY with PYTHON_USE_TYPED_VERB (use python.execute), EXECFILE with EXECFILE_SEND_LINES_INDIVIDUALLY, and DEBUG followed by a crash, hang or memory subcommand with DEBUG_COMMAND_CRASHES_PROCESS / DEBUG_COMMAND_HANGS_PROCESS / DEBUG_COMMAND_EXHAUSTS_MEMORY (DEBUG HITCH / RENDERHITCH are allowed); force:true runs any of them anyway.",
    RPC_PARAMS(
        RPC_PARAM_REQ("command", "string", "Full console command line including arguments, e.g. 'Stat Unit' or 'showflag.Bloom 0'."),
        RPC_PARAM_OPT("world", "string", "Target world selector: 'editor' (the editor world, even while PIE runs), 'server' (first PIE world with authority — listen/dedicated server, or the sole standalone instance), 'client' (first PIE client), 'client:N' (N-th PIE client, 1-based), or 'pie:N' (raw PIEInstance N). Omitted: the editor world when PIE is not running, the PIE world when exactly one runs, TARGET_AMBIGUOUS when several run."),
        RPC_PARAM_DEF("force", "boolean", "Run a refused line anyway: a scalability-CVar set ('sg.<Group> N' or any ECVF_Scalability CVar), accepting that the CVar is pinned at ECVF_SetByConsole and the editor's own Scalability panel can no longer change its group until the editor restarts; or a QUIT_EDITOR / CLOSE_SLATE_MAINFRAME, PY, EXECFILE or DEBUG crash/hang/memory line, accepting that it skips the checks of the typed verb named in the refusal. Ignored for every other command.", "false")
    ))
{
  // A console set of ANY scalability-flagged cvar — the sg.* group or one of the ordinary r.*
  // members the group's ini section drives — pins it at ECVF_SetByConsole for the life of the
  // process, permanently outranking the editor's own Scalability panel: a side effect that
  // outlives this call. Refused in favour of the typed verb that writes at the panel's own
  // priority; see Handlers/ScalabilityConsoleGuard.h. Runs ahead of the GEditor check because
  // whether the line pins a cvar is a fact about the string and the console registry, not about
  // the editor being up.
  FString Command = Ctx.GetString(TEXT("command"));
  if (ScalabilityConsoleGuard::IsScalabilityPinningLine(Command) && !Ctx.GetBool(TEXT("force"), false)) {
    Ctx.SendError(ErrorCodes::ERR_SCALABILITY_CVAR_USE_TYPED_VERB,
        ScalabilityConsoleGuard::MakeScalabilityTypedVerbRefusal(Command));
    return true;
  }
  // QUIT_EDITOR, PY, EXECFILE and DEBUG crash/hang/memory lines bypass the checks of the typed
  // verbs that own those effects; see Handlers/ConsoleCommandGuard.h.
  if (!Ctx.GetBool(TEXT("force"), false)) {
    if (const TOptional<ConsoleCommandGuard::FRefusal> Refusal = ConsoleCommandGuard::FindRefusal(Command)) {
      Ctx.SendError(Refusal->Code, Refusal->Message, Refusal->ToJson());
      return true;
    }
  }

  if (!GEngine) {
    Ctx.SendError(ErrorCodes::ERR_EDITOR_NOT_AVAILABLE, TEXT("Engine not available"));
    return true;
  }

  if (Command.IsEmpty()) {
    Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, TEXT("command parameter is required"));
    return true;
  }

  const FString WorldSelector = Ctx.GetString(TEXT("world"));
  const PieWorldSelector::FParsedSelector Selector = PieWorldSelector::Parse(WorldSelector);
  if (Selector.Kind == PieWorldSelector::ESelectorKind::Invalid) {
    Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, Selector.Error);
    return true;
  }
  const bool bGameProcess = PinWright::IsGameProcess();
  if (bGameProcess && Selector.Kind == PieWorldSelector::ESelectorKind::Editor) {
    Ctx.SendError(ErrorCodes::ERR_EDITOR_ONLY,
        TEXT("world 'editor' does not exist in a -game process; omit 'world' to target the game world."));
    return true;
  }

  // An omitted `world` used to mean the editor world even while a PIE session ran, so a game
  // command answered `consumed` there and did nothing (E-console-command-editor-default-during-pie).
  // Now: no PIE -> editor, one PIE world -> that world, several -> refused (see ResolveOmitted).
  // A game process reports its one game world as the sole context.
  const bool bWorldOmitted = WorldSelector.TrimStartAndEnd().IsEmpty();
  const TArray<PieWorldSelector::FPieContextInfo> PieContexts = FDriveLiveResolver::GatherGameContexts();
  int32 MatchIndex = INDEX_NONE;
  if (bWorldOmitted) {
    switch (PieWorldSelector::ResolveOmitted(PieContexts)) {
    case PieWorldSelector::EOmittedWorld::Ambiguous: {
      TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
      Details->SetStringField(TEXT("availablePieContexts"), PieWorldSelector::DescribeContexts(PieContexts));
      Ctx.SendError(ErrorCodes::ERR_TARGET_AMBIGUOUS,
          FString::Printf(TEXT("'world' was omitted and %d PIE worlds are running, so the target world is "
                               "ambiguous. Pass world: 'server', 'client', 'client:N' or 'pie:N' for a game "
                               "world, or 'editor' for the editor world. Available PIE contexts: %s"),
              PieContexts.Num(), *PieWorldSelector::DescribeContexts(PieContexts)),
          Details);
      return true;
    }
    case PieWorldSelector::EOmittedWorld::SolePie:
      MatchIndex = 0;
      break;
    default:
      break;
    }
  } else if (Selector.Kind != PieWorldSelector::ESelectorKind::Editor) {
    MatchIndex = PieWorldSelector::ResolveSelector(Selector, PieContexts);
    if (MatchIndex == INDEX_NONE) {
      Ctx.SendError(ErrorCodes::ERR_WORLD_NOT_FOUND,
          FString::Printf(TEXT("No PIE world matches selector '%s'. Available PIE contexts: %s"),
              *WorldSelector, *PieWorldSelector::DescribeContexts(PieContexts)));
      return true;
    }
  }

  TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
  UWorld* World = nullptr;
  FString ResolvedSelector = WorldSelector.TrimStartAndEnd();
  if (MatchIndex == INDEX_NONE) {
    World = PinWright::GetEditorWorldOrNull();
    ResolvedSelector = TEXT("editor");
    if (!World) {
      // Was unchecked: Exec(nullptr, ...) ran and the handler still reported
      // "Console command executed". World-scoped commands silently did nothing.
      Ctx.SendError(ErrorCodes::ERR_WORLD_NOT_FOUND, bGameProcess
          ? TEXT("No game world is available to execute the command against.")
          : TEXT("No editor world is available to execute the command against."));
      return true;
    }
  } else {
    const PieWorldSelector::FPieContextInfo& Match = PieContexts[MatchIndex];
    World = Match.World;
    // A game world has no PIE instance: label it by the re-passable role selector.
    const bool bPieMatch = Match.PieInstance != INDEX_NONE;
    if (bWorldOmitted) {
      ResolvedSelector = bPieMatch ? FString::Printf(TEXT("pie:%d"), Match.PieInstance)
          : FString(Match.NetMode == NM_Client ? TEXT("client") : TEXT("server"));
    }
    if (bPieMatch) {
      Resp->SetNumberField(TEXT("pieInstance"), Match.PieInstance);
    }
    Resp->SetStringField(TEXT("kind"), PieWorldSelector::ClassifyNetMode(Match.NetMode));
  }

  // UEditorEngine::Exec -> UEngine::Exec returns false only when NOTHING consumed the
  // line: no exec command matched and IConsoleManager::ProcessUserConsoleInput did not
  // recognise it either (UnrealEngine.cpp:5722 returns true for any cvar get/set/help).
  // So a false here means the command or cvar does not exist — a typo, or a name that
  // only exists in a module this build did not load. The return used to be discarded and
  // the handler answered "Console command executed" regardless, which matters more here
  // than anywhere else: this verb is the fallback an agent reaches for when a typed verb
  // fails, so the documented recovery path could not report failure. Same shape as the
  // honest sibling at Handlers/Editor/ViewportHandler.cpp:434.
  //
  // In a PIE world an unconsumed line is retried through the world's first local player, the
  // route the in-game console takes (ULocalPlayer::Exec: game viewport client, game instance,
  // then UPlayer::Exec over PlayerInput, PlayerController, pawn, HUD, GameMode, CheatManager,
  // GameState, camera manager). GEngine->Exec reaches none of those, so EnableCheats, summon
  // and viewmode used to fail here (E-console-command-player-exec).
  // Virtual: UEditorEngine::Exec in the editor, UGameEngine::Exec in a game process.
  bool bConsumed = GEngine->Exec(World, *Command);
  ULocalPlayer* LocalPlayer = MatchIndex != INDEX_NONE ? GEngine->GetFirstGamePlayer(World) : nullptr;
  const bool bPlayerRoute = !bConsumed && LocalPlayer;
  if (bPlayerRoute) {
    bConsumed = LocalPlayer->Exec(World, *Command, *GLog);
  }
  if (!bConsumed) {
    const FString PlayerNote = MatchIndex == INDEX_NONE
        ? FString(TEXT(" PlayerController / CheatManager / game-viewport exec commands (EnableCheats, summon, "
                       "viewmode) only exist in a PIE world: pass world: 'server' or 'client'."))
        : LocalPlayer
            ? FString(TEXT(" The world's local player chain (game viewport, PlayerController, pawn, HUD, "
                           "GameMode, CheatManager) was tried too."))
            : FString(TEXT(" This PIE world has no local player (a dedicated server), so PlayerController / "
                           "CheatManager exec commands cannot run in it; target a client world."));
    Ctx.SendError(ErrorCodes::ERR_EXEC_FAILED,
        FString::Printf(
            TEXT("No exec command or console variable consumed '%s' in world '%s'. Check the spelling, and "
                 "note that commands owned by an unloaded module are not registered.%s"),
            *Command, *ResolvedSelector, *PlayerNote));
    return true;
  }

  Resp->SetBoolField(TEXT("success"), true);
  Resp->SetStringField(TEXT("command"), Command);
  Resp->SetStringField(TEXT("world"), ResolvedSelector);
  Resp->SetBoolField(TEXT("worldDefaulted"), bWorldOmitted);
  Resp->SetStringField(TEXT("worldPath"), World->GetPathName());
  Resp->SetStringField(TEXT("route"), bPlayerRoute ? TEXT("localPlayer") : TEXT("engine"));
  if (bPlayerRoute && LocalPlayer->PlayerController) {
    Resp->SetStringField(TEXT("playerController"), LocalPlayer->PlayerController->GetName());
  }
  // "consumed", not "executed": Exec's true means a handler claimed the line, which is
  // the strongest thing this call can honestly observe. It is not a claim that the
  // command's own effect succeeded — a cvar set to an out-of-range value is consumed too.
  Resp->SetBoolField(TEXT("consumed"), true);
  Resp->SetStringField(TEXT("message"),
      TEXT("Console command was consumed by an exec handler or console variable. This confirms "
           "the command was recognised, not that its effect succeeded — verify the effect with a "
           "read-back verb."));
  Ctx.SendSuccess(Resp);
  return true;
}

// ---- editor.simulate_input ----
REGISTER_RPC_HANDLER("editor.simulate_input", "editor", "Inject a synthetic keyboard or mouse event. Key events go to a RUNNING PIE GAME when one exists: the session's game viewport is put on the keyboard focus path first, the key then travels the real route (focused in-game UMG widget, then the viewport client, then the possessed player's input stack), and the response names the world / player controller / pawn that received it plus whether the player controller registered it. With no PIE session (or target:'editor') the key goes to the focused editor widget instead. In a -game process keys go to the game world ('editor' is EDITOR_ONLY). mouse_click/mouse_move take screen-space coordinates and always route through Slate.",
    RPC_PARAMS(
        RPC_PARAM_REQ("type", "string", "Event kind: 'key_down', 'key_up', 'mouse_click', or 'mouse_move' (case-insensitive aliases also accepted)."),
        RPC_PARAM_OPT("key", "string", "FKey name for key events, e.g. 'A', 'SpaceBar', 'LeftShift'."),
        RPC_PARAM_OPT("x", "number", "Screen-space X coordinate for mouse events, in pixels."),
        RPC_PARAM_OPT("y", "number", "Screen-space Y coordinate for mouse events, in pixels."),
        RPC_PARAM_OPT("button", "string", "Mouse button for click events: 'left' (default), 'right', or 'middle'."),
        RPC_PARAM_DEF("target", "string", "Key destination: 'auto' (default — the running PIE game when there is one, else the editor), 'game' (the PIE game; PIE_NOT_ACTIVE when PIE is not running), or 'editor' (the focused editor widget). Ignored by mouse events.", "auto"),
        RPC_PARAM_OPT("world", "string", "PIE world selector for key events aimed at the game, same grammar as editor.console_command: 'server', 'client', 'client:N' (1-based), 'pie:N'. Default picks the active game viewport (preferring one that has a player controller).")
    ))
{
  if (!FSlateApplication::IsInitialized()) {
    Ctx.SendError(ErrorCodes::ERR_EDITOR_NOT_AVAILABLE, TEXT("Slate application not available"));
    return true;
  }

  FString InputType = Ctx.GetString(TEXT("type")).ToLower();
  FString Key = Ctx.GetString(TEXT("key"));

  bool bSuccess = false;
  FString Message;

  const bool bKeyDown = (InputType == TEXT("key_down") || InputType == TEXT("keydown"));
  const bool bKeyUp = (InputType == TEXT("key_up") || InputType == TEXT("keyup"));
  if (bKeyDown || bKeyUp) {
    // Key events are routed by FDriveGameInput when a game is running, so a key aimed at a
    // PIE session reaches the possessed pawn's input stack rather than whatever editor widget
    // happens to hold Slate focus (board B-simulate-input-key-events-never-reach-pie-pawn).
    // The reverted branch dispatched a bare FSlateApplication::ProcessKeyDownEvent along the
    // editor's focus path, discarded the handled result, and hardcoded success — so a key sent
    // into a live PIE session changed nothing and still answered {success:true}.
    if (Key.IsEmpty()) {
      Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
          FString::Printf(TEXT("Key parameter required for %s"), *InputType));
      return true;
    }
    const FKey InputKey(*Key);
    if (!EKeys::GetKeyDetails(InputKey).IsValid()) {
      Ctx.SendError(ErrorCodes::ERR_INVALID_KEY, FString::Printf(TEXT("Invalid key: %s"), *Key));
      return true;
    }

    const FString TargetToken = Ctx.GetString(TEXT("target"), TEXT("auto")).ToLower();
    if (TargetToken != TEXT("auto") && TargetToken != TEXT("game") && TargetToken != TEXT("editor")) {
      Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
          FString::Printf(TEXT("Unknown target '%s'. Valid: 'auto' (default), 'game', 'editor'."), *TargetToken));
      return true;
    }
    const FString WorldSelector = Ctx.GetString(TEXT("world"));
    const bool bGameProcess = PinWright::IsGameProcess();
    if (bGameProcess && TargetToken == TEXT("editor")) {
      Ctx.SendError(ErrorCodes::ERR_EDITOR_ONLY,
          TEXT("target 'editor' does not exist in a -game process; use 'auto' or 'game'."));
      return true;
    }

    FDriveGameInputTarget GameTarget;
    bool bHaveGameTarget = false;
    if (TargetToken != TEXT("editor")) {
      FString ResolveErrorCode;
      FString ResolveErrorMessage;
      bHaveGameTarget = FDriveGameInput::ResolveTarget(WorldSelector, GameTarget, ResolveErrorCode, ResolveErrorMessage);
      // 'game' asked for the game explicitly, and so did 'auto' with a named PIE world: neither
      // may silently degrade into an editor keystroke. A game process has no editor to degrade to.
      if (!bHaveGameTarget && (TargetToken == TEXT("game") || !WorldSelector.IsEmpty() || bGameProcess)) {
        Ctx.SendError(ResolveErrorCode, ResolveErrorMessage);
        return true;
      }
    }

    const EDriveKeyAction Action = bKeyDown ? EDriveKeyAction::Down : EDriveKeyAction::Up;
    const TCHAR* Verb = bKeyDown ? TEXT("Key down") : TEXT("Key up");

    TSharedPtr<FJsonObject> KeyResp = MakeShared<FJsonObject>();
    KeyResp->SetStringField(TEXT("type"), InputType);
    KeyResp->SetStringField(TEXT("key"), Key);

    if (bHaveGameTarget) {
      APawn* const Pawn = GameTarget.PlayerController ? GameTarget.PlayerController->GetPawn() : nullptr;
      KeyResp->SetStringField(TEXT("target"), TEXT("game"));
      KeyResp->SetStringField(TEXT("route"), TEXT("viewport_client"));
      KeyResp->SetNumberField(TEXT("pieInstance"), GameTarget.PieInstance);
      KeyResp->SetStringField(TEXT("kind"), PieWorldSelector::ClassifyNetMode(GameTarget.NetMode));
      KeyResp->SetStringField(TEXT("netMode"), PieWorldSelector::NetModeToString(GameTarget.NetMode));
      KeyResp->SetStringField(TEXT("worldPath"), GameTarget.World ? GameTarget.World->GetPathName() : TEXT(""));
      KeyResp->SetStringField(TEXT("map"),
          GameTarget.World ? UWorld::RemovePIEPrefix(GameTarget.World->GetMapName()) : TEXT(""));
      KeyResp->SetStringField(TEXT("playerController"),
          GameTarget.PlayerController ? GameTarget.PlayerController->GetPathName() : TEXT("none"));
      KeyResp->SetStringField(TEXT("pawn"), Pawn ? Pawn->GetPathName() : TEXT("none"));

      if (!GameTarget.PlayerController || !GameTarget.PlayerController->PlayerInput) {
        KeyResp->SetBoolField(TEXT("success"), false);
        Ctx.SendError(ErrorCodes::ERR_INPUT_FAILED,
            TEXT("The selected PIE target has no local UPlayerInput to observe."), KeyResp);
        return true;
      }

      const TSharedRef<FAsyncResponseToken> Token = Ctx.MakeAsyncToken();
      const TSharedRef<TSharedPtr<FAsyncRequestLifetimeLease>> LeaseHolder =
          MakeShared<TSharedPtr<FAsyncRequestLifetimeLease>>();
      const TSharedRef<TWeakPtr<FDriveGameInputRequest>> RequestHolder =
          MakeShared<TWeakPtr<FDriveGameInputRequest>>();
      TSharedPtr<FDriveGameInputRequest> Request = FDriveGameInput::BeginDeliverKey(
          GameTarget, InputKey, Action,
          [Token, KeyResp, LeaseHolder, VerbString = FString(Verb), Key](
              const FDriveGameKeyObservation& Observation)
          {
            if (LeaseHolder->IsValid() && (*LeaseHolder)->IsActive()) {
              (*LeaseHolder)->Release();
            }
            KeyResp->SetBoolField(TEXT("handled"), Observation.bHandled);
            KeyResp->SetBoolField(TEXT("playerInputEventQueued"), Observation.bEventQueued);
            KeyResp->SetNumberField(TEXT("playerInputEventId"), Observation.EventId);
            KeyResp->SetNumberField(TEXT("injectionFrame"), Observation.InjectionFrame);
            KeyResp->SetBoolField(TEXT("deliveredToGame"), Observation.bDeliveredToGame);
            KeyResp->SetStringField(TEXT("consumingRoute"), Observation.ConsumingRoute);
            KeyResp->SetBoolField(TEXT("success"), Observation.bDeliveredToGame);
            if (Observation.bDeliveredToGame) {
              KeyResp->SetStringField(TEXT("message"), FString::Printf(
                  TEXT("%s: %s reached the selected PIE UPlayerInput on its next tick."),
                  *VerbString, *Key));
              Token->SendSuccess(KeyResp);
            } else {
              Token->SendError(ErrorCodes::ERR_INPUT_FAILED, FString::Printf(
                  TEXT("%s '%s' was not processed by the selected PIE UPlayerInput (consumingRoute=%s)."),
                  *VerbString, *Key, *Observation.ConsumingRoute), KeyResp);
            }
          });
      if (!Request.IsValid()) {
        KeyResp->SetBoolField(TEXT("success"), false);
        Ctx.SendError(ErrorCodes::ERR_INPUT_FAILED,
            TEXT("The selected PIE input observation could not be started."), KeyResp);
        return true;
      }
      *RequestHolder = Request;
      *LeaseHolder = Ctx.RetainAsyncRequestLifetime([RequestHolder]()
      {
        if (const TSharedPtr<FDriveGameInputRequest> Pending = RequestHolder->Pin()) {
          Pending->Cancel(TEXT("dispatcher_ended"));
        }
      });
      return true;
    }

    // Editor destination: the focused editor widget, the historical behavior — but with the
    // handled result reported instead of assumed, so "delivered and ignored" is visible.
    bool bHandled = false;
    const bool bInjected = FDriveInput::PressKeyReportingHandled(
        InputKey, EDriveModifierKeys::None, Action, bHandled);
    const FString FocusedWidget = FDriveGameInput::DescribeKeyboardFocus();

    KeyResp->SetStringField(TEXT("target"), TEXT("editor"));
    KeyResp->SetStringField(TEXT("route"), bInjected ? TEXT("slate") : TEXT("none"));
    KeyResp->SetBoolField(TEXT("handled"), bHandled);
    KeyResp->SetStringField(TEXT("focusedWidget"), FocusedWidget);
    KeyResp->SetBoolField(TEXT("success"), bInjected);

    if (!bInjected) {
      Ctx.SendError(ErrorCodes::ERR_INPUT_FAILED,
          TEXT("Slate application is not available to inject the key event"), KeyResp);
      return true;
    }
    KeyResp->SetStringField(TEXT("message"), FString::Printf(
        TEXT("%s: %s -> editor focus path (%s), handled=%s"),
        Verb, *Key, *FocusedWidget, bHandled ? TEXT("true") : TEXT("false")));
    Ctx.SendSuccess(KeyResp);
    return true;
  }

  if (InputType == TEXT("mouse_click") || InputType == TEXT("click")) {
    const FVector2D Point(Ctx.GetNumber(TEXT("x"), 0.0), Ctx.GetNumber(TEXT("y"), 0.0));

    FString Button = Ctx.GetString(TEXT("button"));
    if (Button.IsEmpty()) Button = TEXT("left");

    const EDriveMouseButton MouseButton = FDriveInput::ParseMouseButton(Button);

    // FDriveInput::ClickAt is the vetted injection (native window, effecting button, inactive-
    // input flag, move first), but Slate hit-tests it inside whichever window the platform's
    // window-under-cursor names, and on Linux that cache follows the pointer warp only when SDL's
    // enter event is pumped on a later frame (under -RenderOffscreen's dummy video driver it may not follow at all).
    // A stale cache routes the click into another window, and ProcessMouseButtonDownEvent returns
    // true whatever it hit, so its result proves nothing. So move first and click only once the
    // routing names the top window at the point, waiting a few frames for the platform to catch
    // up; otherwise refuse without clicking.
    if (!FDriveInput::MoveTo(Point)) {
      Ctx.SendError(ErrorCodes::ERR_INPUT_FAILED, TEXT("Slate application is not available to inject the mouse click"));
      return true;
    }
    const TSharedPtr<SWindow> Target = FDriveInput::TopWindowAtPoint(Point);
    if (!Target.IsValid()) {
      Ctx.SendError(ErrorCodes::ERR_INPUT_FAILED, FString::Printf(
          TEXT("No window accepts pointer input at (%.0f, %.0f); nothing was clicked."), Point.X, Point.Y));
      return true;
    }

    // Answers and returns true when the routing names the target (click) or on the last chance
    // (refuse); returns false to keep waiting.
    const auto TryClick = [InputType, Point, MouseButton](const PinWrightSafePoint::FSafePointResponder& Responder,
        const TSharedPtr<SWindow>& TargetWindow, bool bLastChance) -> bool {
      const TSharedPtr<SWindow> Routed = FDriveInput::WindowUnderPoint(Point);
      const FString TargetTitle = TargetWindow->GetTitle().ToString();
      if (Routed != TargetWindow) {
        if (!bLastChance) {
          return false;
        }
        const FString RoutedTitle = Routed.IsValid() ? Routed->GetTitle().ToString() : FString();
        TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
        Details->SetNumberField(TEXT("x"), Point.X);
        Details->SetNumberField(TEXT("y"), Point.Y);
        Details->SetStringField(TEXT("window"), TargetTitle);
        Details->SetStringField(TEXT("routedWindow"), RoutedTitle);
        Responder.SendError(ErrorCodes::ERR_INPUT_FAILED, FString::Printf(
            TEXT("A click at (%.0f, %.0f) would be routed to %s, not to window '%s' on top at that point: the platform's window-under-cursor has not followed the pointer. Nothing was clicked."),
            Point.X, Point.Y,
            Routed.IsValid() ? *FString::Printf(TEXT("window '%s'"), *RoutedTitle) : TEXT("no window"),
            *TargetTitle), Details);
        return true;
      }
      FDriveInput::ClickAt(Point, MouseButton);
      TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
      Resp->SetBoolField(TEXT("success"), true);
      Resp->SetStringField(TEXT("type"), InputType);
      Resp->SetStringField(TEXT("window"), TargetTitle);
      Resp->SetStringField(TEXT("message"), FString::Printf(
          TEXT("Mouse click at (%.0f, %.0f) delivered to window '%s'"), Point.X, Point.Y, *TargetTitle));
      Responder.SendSuccess(Resp);
      return true;
    };

    if (TryClick(PinWrightSafePoint::FSafePointResponder(Ctx), Target, /*bLastChance=*/false)) {
      return true;
    }
    constexpr double ClickRouteWaitSeconds = 0.3;
    const TSharedRef<FAsyncResponseToken> Token = Ctx.MakeAsyncToken();
    const double Deadline = FPlatformTime::Seconds() + ClickRouteWaitSeconds;
    const TWeakPtr<SWindow> WeakTarget = Target;
    FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
        [Token, WeakTarget, Deadline, TryClick](float) -> bool {
          const TSharedPtr<SWindow> TargetWindow = WeakTarget.Pin();
          if (!TargetWindow.IsValid()) {
            Token->SendError(ErrorCodes::ERR_INPUT_FAILED,
                TEXT("The window under the click point closed before the click could be delivered."));
            return false;
          }
          return !TryClick(PinWrightSafePoint::FSafePointResponder(Token), TargetWindow, FPlatformTime::Seconds() >= Deadline);
        }));
    return true;
  }

  if (InputType == TEXT("mouse_move") || InputType == TEXT("move")) {
    double X = Ctx.GetNumber(TEXT("x"), 0.0);
    double Y = Ctx.GetNumber(TEXT("y"), 0.0);

    FSlateApplication& SlateApp = FSlateApplication::Get();
    FVector2D Position((float)X, (float)Y);
    SlateApp.SetCursorPos(Position);

    bSuccess = true;
    Message = FString::Printf(TEXT("Mouse moved to (%f, %f)"), X, Y);
  } else {
    Message = FString::Printf(TEXT("Unknown input type: %s. Supported: key_down, key_up, mouse_click, mouse_move"), *InputType);
  }

  TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
  Resp->SetBoolField(TEXT("success"), bSuccess);
  Resp->SetStringField(TEXT("type"), InputType);
  Resp->SetStringField(TEXT("message"), Message);

  if (bSuccess) {
    Ctx.SendSuccess(Resp);
  } else {
    Ctx.SendError(ErrorCodes::ERR_INPUT_FAILED, Message);
  }
  return true;
}
