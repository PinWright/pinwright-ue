// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"

namespace PinWrightPerformance
{
    // Whether PinWright's background-throttle guard holds the editor CPU throttle off; sampled by
    // performance.run_benchmark. Set at static init by the editor module (it owns the guard); null in game.
    extern PINWRIGHTRUNTIME_API bool (*IsBackgroundThrottleGuardHeld)();
}
