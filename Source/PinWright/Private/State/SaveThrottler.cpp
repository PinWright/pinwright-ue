// Copyright (c) 2026 Alexander Penkin. MIT License.

// Thread-safe asset save throttling implementation
#include "State/SaveThrottler.h"
#include "HAL/PlatformTime.h"
#include "Misc/ScopeLock.h"

bool FSaveThrottler::ShouldSave(const FString& AssetPath)
{
    FScopeLock Lock(&Mutex);

    // Evict entries older than 5 minutes to prevent unbounded growth
    const double Now = FPlatformTime::Seconds();
    constexpr double MaxAge = 300.0;
    for (auto It = RecentSaveTs.CreateIterator(); It; ++It)
    {
        if ((Now - It.Value()) > MaxAge)
        {
            It.RemoveCurrent();
        }
    }

    const double* Last = RecentSaveTs.Find(AssetPath);
    if (!Last)
    {
        return true;
    }
    return (Now - *Last) >= ThrottleSecondsValue;
}

void FSaveThrottler::RecordSave(const FString& AssetPath)
{
    FScopeLock Lock(&Mutex);
    RecentSaveTs.Add(AssetPath, FPlatformTime::Seconds());
}

double FSaveThrottler::GetElapsedSinceLastSave(const FString& Key) const
{
    FScopeLock Lock(&Mutex);
    const double* Last = RecentSaveTs.Find(Key);
    if (!Last)
    {
        return -1.0;
    }
    return FPlatformTime::Seconds() - *Last;
}

void FSaveThrottler::Reset()
{
    FScopeLock Lock(&Mutex);
    RecentSaveTs.Empty();
    ThrottleSecondsValue = 0.5;
}

bool FSaveThrottler::TrySave(const FString& AssetPath, TFunction<bool()> SaveFunc)
{
    // Check throttle under lock
    {
        FScopeLock Lock(&Mutex);
        const double* Last = RecentSaveTs.Find(AssetPath);
        if (Last && (FPlatformTime::Seconds() - *Last) < ThrottleSecondsValue)
        {
            return true; // Throttled -- treat as success
        }
    }

    // Perform save outside the lock to avoid holding the mutex during I/O
    const bool bSaved = SaveFunc();

    if (bSaved)
    {
        FScopeLock Lock(&Mutex);
        RecentSaveTs.Add(AssetPath, FPlatformTime::Seconds());
    }

    return bSaved;
}
