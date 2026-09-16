// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

class ULevelSequence;

namespace LevelSequenceDumpBuilder
{
    PINWRIGHT_API TSharedPtr<FJsonObject> BuildLevelSequenceJson(const ULevelSequence* Sequence);
}
