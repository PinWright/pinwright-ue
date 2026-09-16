// Copyright (c) 2026 Alexander Penkin. MIT License.

// Thread-safe blueprint inflight/busy tracking for PinWright
#pragma once
#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "HAL/CriticalSection.h"

class PINWRIGHT_API FBlueprintTracker
{
public:
    // Thread-safe operations (internal mutex)
    bool IsInflight(const FString& Key) const;
    void MarkInflight(const FString& Key, const TArray<FString>& Assets);
    void ClearInflight(const FString& Key);

    bool IsBusy(const FString& Key) const;
    void MarkBusy(const FString& Key);
    void ClearBusy(const FString& Key);

    bool IsCreateInflight(const FString& Key) const;
    void MarkCreateInflight(const FString& Key);
    void ClearCreateInflight(const FString& Key);
    bool IsCreateStale(const FString& Key) const;

    void RegisterBlueprint(const FString& Key, const TSharedPtr<FJsonObject>& Entry);
    TSharedPtr<FJsonObject> FindBlueprintEntry(const FString& Key) const;

    // Compound create-inflight operations (thread-safe, used by creation handlers).
    // SubscribeOrCreateInflight: If CreateKey already inflight, appends RequestId
    // and returns true (caller should short-circuit). Otherwise creates entry and
    // returns false (caller should proceed with creation).
    bool SubscribeOrCreateInflight(const FString& CreateKey, const FString& RequestId, double Now);

    // DrainCreateInflightSubscribers: Removes CreateKey from inflight maps and
    // returns the list of subscriber request IDs (empty if key not found).
    TArray<FString> DrainCreateInflightSubscribers(const FString& CreateKey);

    // Per-request busy-state tracking for SCS modifications.
    // Moved from UPinWrightSubsystem (Wave 3A).
    const FString& GetCurrentBusyKey() const { return CurrentBusyKey; }
    void SetCurrentBusyKey(const FString& Key) { CurrentBusyKey = Key; }
    bool IsBusyMarked() const { return bBusyMarked; }
    void SetBusyMarked(bool Value) { bBusyMarked = Value; }
    bool IsBusyScheduled() const { return bBusyScheduled; }
    void SetBusyScheduled(bool Value) { bBusyScheduled = Value; }

    // Combined helper: clear the current busy-state and remove from BusySet
    void ClearCurrentBusyState();

    // Mutable stale timeout for create-inflight staleness checks.
    double& StaleTimeoutRef() { return StaleTimeoutSecValue; }

    // Reset all state (used by tests)
    void Reset();

private:
    mutable FCriticalSection Mutex;

    TMap<FString, TArray<FString>> ExistsInflight;
    TMap<FString, TArray<FString>> CreateInflight;
    TMap<FString, double> CreateInflightTs;
    TSet<FString> BusySet;
    TMap<FString, TSharedPtr<FJsonObject>> Registry;

    double StaleTimeoutSecValue = 60.0;

    // Per-request busy-state (single-threaded, game thread only)
    FString CurrentBusyKey;
    bool bBusyMarked = false;
    bool bBusyScheduled = false;
};
