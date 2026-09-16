// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Utils/JobMonitorLog.h"

#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "Logging/LogMacros.h"
#include "Misc/Paths.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

DEFINE_LOG_CATEGORY_STATIC(LogJobMonitorLog, Log, All);

FJobMonitorLog::FJobMonitorLog(FString InPath, int64 InMaxBytes, int32 InKeepRotations)
    : Path(MoveTemp(InPath))
    , MaxBytes(InMaxBytes)
    , KeepRotations(InKeepRotations)
{
    const FString Dir = FPaths::GetPath(Path);
    if (!Dir.IsEmpty() && !IFileManager::Get().DirectoryExists(*Dir))
    {
        IFileManager::Get().MakeDirectory(*Dir, /*Tree=*/true);
    }
    CachedSize = IFileManager::Get().FileSize(*Path);
    if (CachedSize < 0) CachedSize = 0;
}

FJobMonitorLog::~FJobMonitorLog()
{
    FScopeLock Lock(&Mutex);
    CloseHandle_NoLock();
}

void FJobMonitorLog::EnsureHandleOpen_NoLock()
{
    if (Handle.IsValid()) return;

    // bAppend=true keeps existing contents and seeks to end.
    // bAllowRead=true so external `tail -f` processes can read while we hold the write handle.
    IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
    Handle.Reset(PlatformFile.OpenWrite(*Path, /*bAppend=*/true, /*bAllowRead=*/true));
}

void FJobMonitorLog::CloseHandle_NoLock()
{
    if (Handle.IsValid())
    {
        Handle->Flush();
        Handle.Reset();
    }
}

void FJobMonitorLog::AppendEvent(const TSharedRef<FJsonObject>& Line)
{
    FString Serialized;
    {
        TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer
            = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Serialized);
        FJsonSerializer::Serialize(Line, Writer);
    }
    Serialized += TEXT("\n");

    // Encode UTF-8 for on-disk JSONL (matches prior FFileHelper::ForceUTF8WithoutBOM behavior).
    FTCHARToUTF8 Utf8(*Serialized);
    const uint8* Bytes = reinterpret_cast<const uint8*>(Utf8.Get());
    const int64 NumBytes = Utf8.Length();

    FScopeLock Lock(&Mutex);
    RotateIfNeeded_NoLock();
    EnsureHandleOpen_NoLock();
    if (Handle.IsValid())
    {
        Handle->Write(Bytes, NumBytes);
        // Synchronous flush so `tail -f` readers (and the Monitor tool) see this line
        // before the calling RPC returns. Without this, OS buffering on Windows can hold
        // the terminal completed/failed event past the read deadline.
        Handle->Flush();
        CachedSize += NumBytes;
    }
    else if (!bWarnedOpenFailed)
    {
        // One-shot warning so a stuck handle (locked file, missing dir, permissions)
        // is observable in the editor log. Subsequent failures stay silent to avoid spam.
        bWarnedOpenFailed = true;
        UE_LOG(LogJobMonitorLog, Warning,
            TEXT("FJobMonitorLog: failed to open append handle for '%s'; dropping event and silencing further warnings this session."),
            *Path);
    }
}

void FJobMonitorLog::RotateIfNeeded_NoLock()
{
    if (CachedSize < MaxBytes) return;

    // Close the handle before renaming the underlying file; reopened lazily on next write.
    CloseHandle_NoLock();

    // Shift jobs.jsonl.(N-1) -> jobs.jsonl.N, ..., jobs.jsonl -> jobs.jsonl.1.
    for (int32 i = KeepRotations; i >= 1; --i)
    {
        const FString Older = FString::Printf(TEXT("%s.%d"), *Path, i);
        const FString Newer = (i == 1) ? Path : FString::Printf(TEXT("%s.%d"), *Path, i - 1);
        if (IFileManager::Get().FileExists(*Newer))
        {
            IFileManager::Get().Delete(*Older, /*RequireExists=*/false, /*EvenReadOnly=*/false);
            IFileManager::Get().Move(*Older, *Newer, /*bReplace=*/true);
        }
    }
    CachedSize = 0;
}
