// Copyright (c) 2026 Alexander Penkin. MIT License.

// misc.set_game_speed: the game-safe verb of the editor module's MiscHandler.cpp.

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Dispatch/PinWrightWorld.h"
#include "Dom/JsonObject.h"
#include "Engine/World.h"
#include "GameFramework/WorldSettings.h"

// ---- misc.set_game_speed ----
REGISTER_RPC_HANDLER("misc.set_game_speed", "misc", "Set the global TimeDilation on the active world (1.0 = real-time, 0.5 = half-speed slow-mo, 2.0 = double-speed).",
    RPC_PARAMS(
        RPC_PARAM_OPT("speed", "number", "Time dilation factor (default 1.0, range 0-100)")
    ))
{
    double Speed = Ctx.GetNumber(TEXT("speed"), 1.0);

    if (Speed < 0.0 || Speed > 100.0)
    {
        Ctx.SendError(TEXT("INVALID_PARAMS"), TEXT("Speed must be between 0.0 and 100.0"));
        return true;
    }

    UWorld* World = PinWright::GetTargetWorld();

    if (!World)
    {
        Ctx.SendError(TEXT("NO_WORLD"), TEXT("No world available"));
        return true;
    }

    AWorldSettings* WorldSettings = World->GetWorldSettings();
    if (!WorldSettings)
    {
        Ctx.SendError(TEXT("NO_WORLD_SETTINGS"), TEXT("World settings not available"));
        return true;
    }

    WorldSettings->SetTimeDilation(static_cast<float>(Speed));

    TSharedPtr<FJsonObject> ResponseJson = MakeShared<FJsonObject>();
    ResponseJson->SetNumberField(TEXT("speed"), Speed);
    ResponseJson->SetNumberField(TEXT("actualTimeDilation"), WorldSettings->TimeDilation);
    Ctx.SendSuccess(ResponseJson);
    return true;
}
