// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

class UAnimSequence;
class UAnimSequenceBase;

namespace AnimSequenceDumpBuilder
{
    PINWRIGHT_API TSharedPtr<FJsonObject> BuildAnimSequenceJson(const UAnimSequence* Sequence);
    PINWRIGHT_API TArray<TSharedPtr<FJsonValue>> BuildCurvesArrayJson(const UAnimSequence* Sequence);
    // Per-bone-track readback: one {boneName, keyCount} entry per raw bone track authored via
    // add_bone_track. Raw bone tracks are NOT curves (BuildCurvesArrayJson never surfaces them),
    // so this is the only structured readback of which bones were keyed and how many.
    PINWRIGHT_API TArray<TSharedPtr<FJsonValue>> BuildBoneTracksArrayJson(const UAnimSequence* Sequence);
    PINWRIGHT_API TArray<TSharedPtr<FJsonValue>> BuildNotifiesArrayJson(const UAnimSequenceBase* Sequence);
    PINWRIGHT_API TArray<TSharedPtr<FJsonValue>> BuildSyncMarkersArrayJson(const UAnimSequence* Sequence);
}
