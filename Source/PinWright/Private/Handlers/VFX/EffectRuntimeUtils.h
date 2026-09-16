// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
// TWeakObjectPtr's storage member needs the FWeakObjectPtr definition, which CoreMinimal.h does
// not pull in; without this the members below only compile inside a unity blob.
#include "UObject/WeakObjectPtr.h"

class AActor;
class UNiagaraComponent;

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

    bool ResolveTarget(const FString& SystemName, FNiagaraTarget& OutTarget,
        FString& OutErrorCode, FString& OutErrorMessage);

    bool Activate(const FNiagaraTarget& Target, bool bReset,
        FString& OutErrorCode, FString& OutErrorMessage);

    bool Advance(const FNiagaraTarget& Target, int32 Steps, float DeltaTime,
        FAdvanceResult& OutResult,
        FString& OutErrorCode, FString& OutErrorMessage);
}
