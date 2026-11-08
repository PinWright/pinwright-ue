// Copyright (c) 2026 Alexander Penkin. MIT License.

// system.console_command: the game-safe half of the editor module's SystemControlHandler.cpp,
// which keeps system.run_tests.

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ParamSpec.h"
#include "Handlers/ScalabilityConsoleGuard.h"
#include "Handlers/ConsoleCommandGuard.h"
#include "Dispatch/PinWrightWorld.h"
#include "Dom/JsonObject.h"
#include "Engine/Engine.h"
#include "Engine/World.h"

// ---- system.console_command ----
REGISTER_RPC_HANDLER("system.console_command", "system", "Run a console command at the process / GEngine scope. Distinct from editor.console_command which targets the editor world; use this for project / engine-wide commands. A line that SETS a scalability CVar is REFUSED with SCALABILITY_CVAR_USE_TYPED_VERB — either an 'sg.*' group, or any CVar carrying ECVF_Scalability / ECVF_ScalabilityGroup (r.ViewDistanceScale, r.Streaming.PoolSize, r.ScreenPercentage, r.MaxAnisotropy, ...), because a console set pins it at ECVF_SetByConsole above the ECVF_SetByScalability priority the editor's own Settings > Engine Scalability Settings panel writes at, for the rest of the session. Use performance.set_scalability, or pass force:true to accept the pin. READING such a CVar (its name with no value) is not refused, and neither is the aggregate 'scalability N', which routes through Scalability::SetQualityLevels at the panel's own priority. A line whose first command word is QUIT_EDITOR or CLOSE_SLATE_MAINFRAME is REFUSED with EDITOR_QUIT_USE_TYPED_VERB (use editor.quit), PY with PYTHON_USE_TYPED_VERB (use python.execute), EXECFILE with EXECFILE_SEND_LINES_INDIVIDUALLY, and DEBUG followed by a crash, hang or memory subcommand with DEBUG_COMMAND_CRASHES_PROCESS / DEBUG_COMMAND_HANGS_PROCESS / DEBUG_COMMAND_EXHAUSTS_MEMORY (DEBUG HITCH / RENDERHITCH are allowed); force:true runs any of them anyway.",
    RPC_PARAMS(
        RPC_PARAM_REQ("command", "string", "Full console command line including arguments, e.g. 'log LogStreaming Verbose' or 'stat unit'."),
        RPC_PARAM_DEF("force", "boolean", "Run a refused line anyway: a scalability-CVar set ('sg.<Group> N' or any ECVF_Scalability CVar), accepting that the CVar is pinned at ECVF_SetByConsole and the editor's own Scalability panel can no longer change its group until the editor restarts; or a QUIT_EDITOR / CLOSE_SLATE_MAINFRAME, PY, EXECFILE or DEBUG crash/hang/memory line, accepting that it skips the checks of the typed verb named in the refusal. Ignored for every other command.", "false")
    ))
{
    auto* Payload = Ctx.GetRawPayload().Get();
    if (!Payload)
    {
        Ctx.SendError(TEXT("INVALID_PAYLOAD"), TEXT("system.console_command payload missing."));
        return true;
    }

    // Top-level 'command' only. The nested params.command envelope this used to fall back
    // to was unreachable twice over: 'params' is not a declared parameter, and 'command' is
    // required, so a request carrying only the envelope is refused by the dispatcher's
    // required-param gate before the body runs.
    FString Cmd;
    Payload->TryGetStringField(TEXT("command"), Cmd);
    if (Cmd.IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("command required"));
        return true;
    }

    // A console set of ANY scalability-flagged cvar — the sg.* group or one of the ordinary
    // r.* members the group's ini section drives — pins it at ECVF_SetByConsole for the life of
    // the process, permanently outranking the editor's own Scalability panel: a side effect that
    // outlives this call and is not this caller's to spend. Refused in favour of the typed verb
    // that writes at the panel's own priority; see Handlers/ScalabilityConsoleGuard.h. Runs ahead
    // of the engine check because it is a fact about the string and the console registry, not
    // about the editor being up.
    if (ScalabilityConsoleGuard::IsScalabilityPinningLine(Cmd) && !Ctx.GetBool(TEXT("force"), false))
    {
        Ctx.SendError(TEXT("SCALABILITY_CVAR_USE_TYPED_VERB"),
            ScalabilityConsoleGuard::MakeScalabilityTypedVerbRefusal(Cmd));
        return true;
    }
    // QUIT_EDITOR, PY, EXECFILE and DEBUG crash/hang/memory lines bypass the checks of the typed
    // verbs that own those effects; see Handlers/ConsoleCommandGuard.h.
    if (!Ctx.GetBool(TEXT("force"), false))
    {
        if (const TOptional<ConsoleCommandGuard::FRefusal> Refusal = ConsoleCommandGuard::FindRefusal(Cmd))
        {
            Ctx.SendError(Refusal->Code, Refusal->Message, Refusal->ToJson());
            return true;
        }
    }

    if (!GEngine)
    {
        Ctx.SendError(TEXT("EDITOR_NOT_AVAILABLE"), TEXT("Engine not available"));
        return true;
    }

    bool bExecCalled = false;
    bool bOk = false;

    // Prefer executing with a valid world context where possible to avoid assertions inside
    // engine helpers that require a proper world (e.g. when running Open/Map commands): the
    // editor world in the editor (GEngine is the editor engine there, so Exec is the editor's),
    // the game world in a game process.
    UWorld* TargetWorld = PinWright::IsGameProcess()
        ? PinWright::GetTargetWorld()
        : PinWright::GetEditorWorldOrNull();

    if (TargetWorld)
    {
        bOk = GEngine->Exec(TargetWorld, *Cmd);
        bExecCalled = true;
    }

    // Fallback: try all known engine world contexts if the editor world
    // did not handle the command successfully.
    if (!bOk && GEngine)
    {
        for (const FWorldContext& WCtx : GEngine->GetWorldContexts())
        {
            UWorld* World = WCtx.World();
            if (!World) continue;
            const bool bWorldOk = GEngine->Exec(World, *Cmd);
            bExecCalled = bExecCalled || bWorldOk;
            if (bWorldOk)
            {
                bOk = true;
                break;
            }
        }
    }

    // If we could not find any valid world to execute against, avoid
    // invoking the engine command path entirely and return a structured
    // error instead of risking an assertion.
    if (!bExecCalled && !TargetWorld)
    {
        TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
        Out->SetStringField(TEXT("command"), Cmd);
        Out->SetBoolField(TEXT("success"), false);
        Ctx.SendError(TEXT("EDITOR_WORLD_NOT_AVAILABLE"), TEXT("Editor world not available for command"));
        return true;
    }

    TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
    Out->SetStringField(TEXT("command"), Cmd);
    Out->SetBoolField(TEXT("success"), bOk);
    if (bOk)
    {
        Ctx.SendSuccess(Out);
    }
    else
    {
        Ctx.SendError(TEXT("EXEC_FAILED"), TEXT("Command not executed"));
    }
    return true;
}
