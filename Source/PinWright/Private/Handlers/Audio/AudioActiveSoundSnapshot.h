// Copyright (c) 2026 Alexander Penkin. MIT License.

// One row of audio.list_active_sounds, built from an engine FActiveSound.
//
// Split out of the handler so a test can drive the row builder with a hand-made FActiveSound on a
// host that has no audio device (the suite runs with -nosound), while the handler itself is only
// observable end to end where a device exists.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

struct FActiveSound;

namespace PwActiveSoundSnapshot
{
    // Must run on the game thread with the audio thread suspended (or not running): it reads the
    // audio-thread-owned FActiveSound and resolves the owning UAudioComponent by id.
    // bVirtualized is the container the caller found the sound in (the device's active list vs its
    // virtual-loop map), not something re-derived from the sound's own flags.
    TSharedRef<FJsonObject> DescribeActiveSound(const FActiveSound& ActiveSound, uint32 DeviceId,
        bool bVirtualized);
}
