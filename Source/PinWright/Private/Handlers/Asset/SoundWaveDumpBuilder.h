// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

class USoundWave;

namespace SoundWaveDumpBuilder
{
    PINWRIGHT_API int32 GetRawSampleRate(const USoundWave* Wave);
    PINWRIGHT_API TSharedPtr<FJsonObject> BuildSoundWaveJson(const USoundWave* Wave);
}
