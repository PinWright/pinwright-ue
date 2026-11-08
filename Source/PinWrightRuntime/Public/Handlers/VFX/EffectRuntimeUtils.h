// Copyright (c) 2026 Alexander Penkin. MIT License.

// Shared by the runtime effect.* verbs (PinWrightRuntime Handlers/VFX/EffectHandler.cpp) and the
// editor-only ones (PinWright Handlers/VFX/EffectHandler.cpp: step_and_capture, spawn_niagara).

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
// TWeakObjectPtr's storage member needs the FWeakObjectPtr definition, which CoreMinimal.h does
// not pull in; without this the members below only compile inside a unity blob.
#include "UObject/WeakObjectPtr.h"

#include <cmath>

class AActor;
class UNiagaraComponent;
class UWorld;

namespace PinWrightEffectStepAndCaptureLimits
{
    constexpr double MinActualStepDeltaSeconds = 1.0e-4;
    constexpr int32 MaxSteps = 10000;
    constexpr double MaxSimulatedSeconds = 60.0;

    struct FPreparedSimulation
    {
        int32 Steps = 0;
        float SimulationDeltaTime = 0.0f;
    };

    inline FString Describe()
    {
        return TEXT("Limits: actual per-step delta >= 0.0001 seconds, steps <= 10000, "
                    "total simulated duration <= 60 seconds.");
    }

    inline bool Prepare(const double RequestedSteps, const double StepDeltaSeconds,
        FPreparedSimulation& OutSimulation, FString& OutError)
    {
        if (!FMath::IsFinite(RequestedSteps) || RequestedSteps < 1.0 ||
            RequestedSteps > static_cast<double>(MaxSteps) ||
            RequestedSteps != FMath::RoundToDouble(RequestedSteps))
        {
            OutError = TEXT("steps must be a finite positive whole number within the supported step count.");
            return false;
        }

        const int32 Steps = static_cast<int32>(FMath::RoundToDouble(RequestedSteps));
        if (!FMath::IsFinite(StepDeltaSeconds) ||
            StepDeltaSeconds < MinActualStepDeltaSeconds)
        {
            OutError = TEXT("deltaTime is non-finite or below the supported minimum.");
            return false;
        }

        if (StepDeltaSeconds > MaxSimulatedSeconds / static_cast<double>(Steps))
        {
            OutError = TEXT("steps multiplied by deltaTime exceeds the supported total simulated duration.");
            return false;
        }

        // AdvanceSimulation accepts float. Keep the actual value at or above the minimum;
        // otherwise round down when nearest-float conversion would cross the requested duration
        // or the duration cap.
        float SimulationDeltaTime = static_cast<float>(StepDeltaSeconds);
        if (static_cast<double>(SimulationDeltaTime) < MinActualStepDeltaSeconds)
        {
            SimulationDeltaTime = std::nextafter(SimulationDeltaTime, 1.0f);
        }
        else if (static_cast<double>(SimulationDeltaTime) > StepDeltaSeconds)
        {
            SimulationDeltaTime = std::nextafter(SimulationDeltaTime, 0.0f);
        }

        const double ActualStepDeltaSeconds = static_cast<double>(SimulationDeltaTime);
        if (!FMath::IsFinite(SimulationDeltaTime) ||
            ActualStepDeltaSeconds < MinActualStepDeltaSeconds)
        {
            OutError = TEXT("the actual float per-step delta is below the supported minimum.");
            return false;
        }

        if (ActualStepDeltaSeconds > MaxSimulatedSeconds / static_cast<double>(Steps))
        {
            OutError = TEXT("the actual step count and per-step delta exceed the supported total simulated duration.");
            return false;
        }

        OutSimulation.Steps = Steps;
        OutSimulation.SimulationDeltaTime = SimulationDeltaTime;
        return true;
    }
}

namespace PinWrightEffectRuntime
{
    struct FNiagaraTarget
    {
        TWeakObjectPtr<AActor> Actor;
        TWeakObjectPtr<UNiagaraComponent> Component;
    };

    struct FAdvanceResult
    {
        double SimulatedSeconds = 0.0;
        bool bStoppedEarly = false;
        FString StopReason;
    };

    // Editor: the outliner label. Game: the label where the build keeps labels, else the object name.
    PINWRIGHTRUNTIME_API FString GetActorLabel(const AActor& Actor);

    // Every live actor of World except its WorldSettings (the game-process stand-in for
    // UEditorActorSubsystem::GetAllLevelActors).
    PINWRIGHTRUNTIME_API TArray<AActor*> GetWorldActors(UWorld& World);

    // Location from the payload: array [x,y,z] or object {x,y,z}; Default when absent.
    PINWRIGHTRUNTIME_API FVector ParseLocationFromPayload(const TSharedPtr<FJsonObject>& Payload,
        const FString& FieldName, FVector Default = FVector::ZeroVector);

    // Editor: the editor world's level actors (never PIE). Game: the game world.
    PINWRIGHTRUNTIME_API bool ResolveTarget(const FString& SystemName, FNiagaraTarget& OutTarget,
        FString& OutErrorCode, FString& OutErrorMessage);

    PINWRIGHTRUNTIME_API bool Activate(const FNiagaraTarget& Target, bool bReset,
        FString& OutErrorCode, FString& OutErrorMessage);

    PINWRIGHTRUNTIME_API bool Advance(const FNiagaraTarget& Target, int32 Steps, float DeltaTime,
        FAdvanceResult& OutResult,
        FString& OutErrorCode, FString& OutErrorMessage);
}
