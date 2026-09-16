// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/CharacterMovementComponent.h"

namespace PinWrightMovementPrediction
{
    inline bool TryParseNetworkSmoothingMode(const FString& Value, ENetworkSmoothingMode& OutMode)
    {
        if (Value.Equals(TEXT("Disabled"), ESearchCase::IgnoreCase))
        {
            OutMode = ENetworkSmoothingMode::Disabled;
            return true;
        }
        if (Value.Equals(TEXT("Linear"), ESearchCase::IgnoreCase))
        {
            OutMode = ENetworkSmoothingMode::Linear;
            return true;
        }
        if (Value.Equals(TEXT("Exponential"), ESearchCase::IgnoreCase))
        {
            OutMode = ENetworkSmoothingMode::Exponential;
            return true;
        }
        return false;
    }

    inline void ApplyMovementPredictionSettings(UCharacterMovementComponent& Movement,
        ENetworkSmoothingMode SmoothingMode, float MaxSmoothUpdateDistance,
        float NoSmoothUpdateDistance)
    {
        Movement.NetworkSmoothingMode = SmoothingMode;
        Movement.NetworkMaxSmoothUpdateDistance = MaxSmoothUpdateDistance;
        Movement.NetworkNoSmoothUpdateDistance = NoSmoothUpdateDistance;
    }
}
