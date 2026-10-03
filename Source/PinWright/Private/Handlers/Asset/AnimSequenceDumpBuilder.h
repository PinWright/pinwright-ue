// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

class UAnimSequence;
class UAnimSequenceBase;
struct FFloatCurve;
struct FTransformCurve;

namespace AnimSequenceDumpBuilder
{
    // Every plugin curve read goes through these. The data model is the editor authority;
    // UAnimSequenceBase::GetCurveData() is the runtime RawCurveData copy, synced from the model only
    // outside controller brackets (and with redundant keys stripped on load), so it can be empty or
    // stale for a clip being authored. Falls back to that copy only when the model is not valid.
    PINWRIGHT_API const TArray<FFloatCurve>& GetAuthoredFloatCurves(const UAnimSequenceBase* Sequence);
    PINWRIGHT_API const TArray<FTransformCurve>& GetAuthoredTransformCurves(const UAnimSequenceBase* Sequence);
    PINWRIGHT_API TSharedPtr<FJsonObject> BuildAnimSequenceJson(const UAnimSequence* Sequence);
    PINWRIGHT_API TArray<TSharedPtr<FJsonValue>> BuildCurvesArrayJson(const UAnimSequence* Sequence);
    // Per-bone-track readback: one {boneName, keyCount} entry per raw bone track authored via
    // add_bone_track. Raw bone tracks are NOT curves (BuildCurvesArrayJson never surfaces them),
    // so this is the only structured readback of which bones were keyed and how many.
    PINWRIGHT_API TArray<TSharedPtr<FJsonValue>> BuildBoneTracksArrayJson(const UAnimSequence* Sequence);
    PINWRIGHT_API TArray<TSharedPtr<FJsonValue>> BuildNotifiesArrayJson(const UAnimSequenceBase* Sequence);
    PINWRIGHT_API TArray<TSharedPtr<FJsonValue>> BuildSyncMarkersArrayJson(const UAnimSequence* Sequence);
}
