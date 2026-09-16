// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#if WITH_DEV_AUTOMATION_TESTS
namespace PinWrightEffectSpawnTestHooks
{
    // Narrow test-only seam: production always observes the component immediately after its
    // single Activate call. Tests may force that real component inactive before the observation.
    inline bool& ForceInactiveAfterActivation()
    {
        static bool bForceInactive = false;
        return bForceInactive;
    }
}
#endif
