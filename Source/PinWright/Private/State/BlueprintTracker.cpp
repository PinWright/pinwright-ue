// Copyright (c) 2026 Alexander Penkin. MIT License.

// Thread-safe blueprint inflight/busy tracking implementation
#include "State/BlueprintTracker.h"
#include "HAL/PlatformTime.h"
#include "Misc/ScopeLock.h"

// ============================================================================
// Exists-Inflight tracking
// ============================================================================

bool FBlueprintTracker::IsInflight(const FString& Key) const
{
    FScopeLock Lock(&Mutex);
    return ExistsInflight.Contains(Key);
}

void FBlueprintTracker::MarkInflight(const FString& Key, const TArray<FString>& Assets)
{
    FScopeLock Lock(&Mutex);
    ExistsInflight.Add(Key, Assets);
}

void FBlueprintTracker::ClearInflight(const FString& Key)
{
    FScopeLock Lock(&Mutex);
    ExistsInflight.Remove(Key);
}

// ============================================================================
// Busy tracking
// ============================================================================

bool FBlueprintTracker::IsBusy(const FString& Key) const
{
    FScopeLock Lock(&Mutex);
    return BusySet.Contains(Key);
}

void FBlueprintTracker::MarkBusy(const FString& Key)
{
    FScopeLock Lock(&Mutex);
    BusySet.Add(Key);
}

void FBlueprintTracker::ClearBusy(const FString& Key)
{
    FScopeLock Lock(&Mutex);
    BusySet.Remove(Key);
}

// ============================================================================
// Create-Inflight tracking
// ============================================================================

bool FBlueprintTracker::IsCreateInflight(const FString& Key) const
{
    FScopeLock Lock(&Mutex);
    return CreateInflight.Contains(Key);
}

void FBlueprintTracker::MarkCreateInflight(const FString& Key)
{
    FScopeLock Lock(&Mutex);
    if (!CreateInflight.Contains(Key))
    {
        CreateInflight.Add(Key, TArray<FString>());
        CreateInflightTs.Add(Key, FPlatformTime::Seconds());
    }
}

void FBlueprintTracker::ClearCreateInflight(const FString& Key)
{
    FScopeLock Lock(&Mutex);
    CreateInflight.Remove(Key);
    CreateInflightTs.Remove(Key);
}

bool FBlueprintTracker::IsCreateStale(const FString& Key) const
{
    FScopeLock Lock(&Mutex);
    const double* Ts = CreateInflightTs.Find(Key);
    if (!Ts)
    {
        return false;
    }
    return (FPlatformTime::Seconds() - *Ts) > StaleTimeoutSecValue;
}

// ============================================================================
// Compound Create-Inflight Operations
// ============================================================================

bool FBlueprintTracker::SubscribeOrCreateInflight(const FString& CreateKey, const FString& RequestId, double Now)
{
    FScopeLock Lock(&Mutex);
    if (CreateInflight.Contains(CreateKey))
    {
        CreateInflight[CreateKey].Add(RequestId);
        return true;
    }

    CreateInflight.Add(CreateKey, TArray<FString>());
    CreateInflightTs.Add(CreateKey, Now);
    CreateInflight[CreateKey].Add(RequestId);
    return false;
}

TArray<FString> FBlueprintTracker::DrainCreateInflightSubscribers(const FString& CreateKey)
{
    FScopeLock Lock(&Mutex);
    TArray<FString> Subscribers;
    if (TArray<FString>* Subs = CreateInflight.Find(CreateKey))
    {
        Subscribers = MoveTemp(*Subs);
        CreateInflight.Remove(CreateKey);
        CreateInflightTs.Remove(CreateKey);
    }
    return Subscribers;
}

// ============================================================================
// Per-request Busy State
// ============================================================================

void FBlueprintTracker::ClearCurrentBusyState()
{
    FScopeLock Lock(&Mutex);
    if (!CurrentBusyKey.IsEmpty() && BusySet.Contains(CurrentBusyKey))
    {
        BusySet.Remove(CurrentBusyKey);
    }
    bBusyMarked = false;
    bBusyScheduled = false;
    CurrentBusyKey.Empty();
}

// ============================================================================
// Blueprint Registry
// ============================================================================

void FBlueprintTracker::RegisterBlueprint(const FString& Key, const TSharedPtr<FJsonObject>& Entry)
{
    FScopeLock Lock(&Mutex);
    Registry.Add(Key, Entry);
}

TSharedPtr<FJsonObject> FBlueprintTracker::FindBlueprintEntry(const FString& Key) const
{
    FScopeLock Lock(&Mutex);
    const TSharedPtr<FJsonObject>* Found = Registry.Find(Key);
    if (Found && Found->IsValid())
    {
        return *Found;
    }
    return nullptr;
}

// ============================================================================
// Reset
// ============================================================================

void FBlueprintTracker::Reset()
{
    FScopeLock Lock(&Mutex);
    ExistsInflight.Empty();
    CreateInflight.Empty();
    CreateInflightTs.Empty();
    BusySet.Empty();
    Registry.Empty();
    StaleTimeoutSecValue = 60.0;
    CurrentBusyKey.Empty();
    bBusyMarked = false;
    bBusyScheduled = false;
}
