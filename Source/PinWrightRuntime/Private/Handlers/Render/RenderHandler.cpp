// Copyright (c) 2026 Alexander Penkin. MIT License.

// RenderHandler.cpp (runtime module): the game-safe render.* verbs. The editor-only render.* verbs
// (asset/mesh previews, level-viewport capture, render target assets, Nanite rebuild) stay in
// PinWright/Private/Handlers/Render/RenderHandler.cpp.

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ErrorCodes.h"
#include "Dispatch/PinWrightWorld.h"
#include "Dom/JsonObject.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Utils/RenderingAvailability.h"

// ---- render.lumen_update_scene ----
REGISTER_RPC_HANDLER("render.lumen_update_scene", "render", "Trigger a Lumen scene recapture",
    RPC_NO_PARAMS)
{
    if (!PinWrightRendering::RequireRenderer(Ctx))
    {
        return true;
    }

    // Editor: the editor world (never PIE), as before the runtime split; game: the game world.
    UWorld* World = PinWright::IsGameProcess() ? PinWright::GetTargetWorld() : PinWright::GetEditorWorldOrNull();
    if (World)
    {
        // r.LumenScene.SurfaceCache.Reset 1 is the real one-shot Lumen
        // recapture lever: the renderer resets all atlases/captured cards on
        // the next frame and auto-clears the cvar back to 0
        // (Engine/Source/Runtime/Renderer/Private/Lumen/LumenSceneRendering.cpp).
        // The former r.Lumen.Scene.Recapture does not exist in UE 5.x, so Exec
        // returned false and the handler reported a recapture that never ran.
        // Honor the Exec bool and fail loud on rejection, matching system.console_command.
        const TCHAR* Command = TEXT("r.LumenScene.SurfaceCache.Reset 1");
        const bool bOk = GEngine->Exec(World, Command);
        if (!bOk)
        {
            Ctx.SendError(ErrorCodes::ERR_EXEC_FAILED, TEXT("Command not executed"));
            return true;
        }
        // The !bOk early return above already encoded the false case as
        // EXEC_FAILED, so reaching here means bOk is true.
        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetStringField(TEXT("action"), TEXT("lumen_update_scene"));
        Result->SetStringField(TEXT("command"), Command);
        Result->SetBoolField(TEXT("executed"), true);
        Ctx.SendSuccess(Result);
        return true;
    }
    Ctx.SendError(ErrorCodes::ERR_EXECUTION_FAILED, TEXT("Could not execute command (no world context)."));
    return true;
}
