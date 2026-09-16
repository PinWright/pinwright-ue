// Copyright (c) 2026 Alexander Penkin. MIT License.

// Thread-safe asset save throttling for PinWright
#pragma once
#include "CoreMinimal.h"
#include "HAL/CriticalSection.h"

class PINWRIGHT_API FSaveThrottler
{
public:
    // Returns true if enough time has passed since last save of this asset
    // (also evicts stale entries older than 5 minutes)
    bool ShouldSave(const FString& AssetPath);

    // Record that this asset was just saved
    void RecordSave(const FString& AssetPath);

    // Combined: save only if not throttled, returns true if save happened
    bool TrySave(const FString& AssetPath, TFunction<bool()> SaveFunc);

    // Check whether save is throttled for the given key (returns elapsed time, or -1 if no record)
    double GetElapsedSinceLastSave(const FString& Key) const;

    // Mutable throttle interval in seconds.
    double& ThrottleSecondsRef() { return ThrottleSecondsValue; }

    // Reset all state (used by tests)
    void Reset();

private:
    mutable FCriticalSection Mutex;
    TMap<FString, double> RecentSaveTs;

    double ThrottleSecondsValue = 0.5;
};
