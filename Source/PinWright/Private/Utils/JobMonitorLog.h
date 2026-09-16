// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "HAL/CriticalSection.h"
#include "GenericPlatform/GenericPlatformFile.h"

namespace JobMonitorLog
{
    // Project-relative path of the shared JSONL event file.
    inline const TCHAR* JobsJsonlRelativePath = TEXT("Saved/PinWright/jobs.jsonl");
}

class PINWRIGHT_API FJobMonitorLog
{
public:
    FJobMonitorLog(FString InPath, int64 InMaxBytes, int32 InKeepRotations);
    ~FJobMonitorLog();

    // Appends one JSON object as a single line + '\n'. Thread-safe.
    // Each call flushes synchronously so external `tail -f` readers see
    // every event including the terminal completed/failed/cancelled line.
    void AppendEvent(const TSharedRef<FJsonObject>& Line);

    // Path of the current primary log file.
    const FString& GetPath() const { return Path; }

private:
    void RotateIfNeeded_NoLock();
    // Opens (or reopens, post-rotation) the append handle. Caller holds Mutex.
    void EnsureHandleOpen_NoLock();
    void CloseHandle_NoLock();

    FString Path;
    int64 MaxBytes;
    int32 KeepRotations;
    int64 CachedSize = -1;   // -1 = not yet probed
    FCriticalSection Mutex;
    TUniquePtr<IFileHandle> Handle;
    // Latches true the first time we fail to open the append handle so the
    // observability warning is emitted once per session, not on every event.
    bool bWarnedOpenFailed = false;
};
