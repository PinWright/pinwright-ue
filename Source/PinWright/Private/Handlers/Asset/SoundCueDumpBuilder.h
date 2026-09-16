// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

class USoundCue;
class USoundNode;
class USoundNodeWavePlayer;

namespace SoundCueDumpBuilder
{
    // One per-child array a USoundNode keeps parallel to ChildNodes: USoundNodeRandom::Weights,
    // USoundNodeMixer / USoundNodeConcatenator::InputVolume, USoundNodeGroupControl::GroupSizes,
    // USoundNodeDistanceCrossFade::CrossFadeInput. The engine marks every one of them
    // `editfixedsize` because only USoundNode::InsertChildNode is allowed to resize it, which is
    // exactly what makes an array that drifted out of step with ChildNodes a defect worth
    // reporting (a zero-weight Random always plays child 0; a short Mixer InputVolume reads out
    // of bounds at playback). Values are filled for numeric element types only; Num is the real
    // array length whatever the element type.
    struct FSoundNodeChildArray
    {
        FName PropertyName;
        int32 Num = 0;
        bool bNumeric = false;
        TArray<double> Values;
    };

    PINWRIGHT_API FString NormalizeSoundCuePath(const FString& Path);
    PINWRIGHT_API USoundCue* LoadSoundCueFromPath(const FString& CuePath, FString* OutNormalizedPath = nullptr);
    PINWRIGHT_API TSharedPtr<FJsonObject> BuildSoundCueJson(const USoundCue* Cue);
    PINWRIGHT_API FString ResolveSoundNodeWavePlayerPath(const USoundNodeWavePlayer* WavePlayer);
    PINWRIGHT_API void GetSoundNodeChildArrays(const USoundNode* Node, TArray<FSoundNodeChildArray>& OutArrays);
}
