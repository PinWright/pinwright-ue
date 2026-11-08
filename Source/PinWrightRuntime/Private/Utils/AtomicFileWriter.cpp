// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Utils/AtomicFileWriter.h"
#include "Utils/AtomicFileWriterInternal.h"

#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "HAL/PlatformMisc.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Containers/StringConv.h"

#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#elif PLATFORM_LINUX
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace AtomicFileWriter::Private
{
    namespace
    {
        constexpr int32 MaxTempNameAttempts = 16;
        constexpr int32 MaxPublishAttempts = 4;
        constexpr int32 VerificationChunkSize = 64 * 1024;

#if PLATFORM_WINDOWS
        FString FormatWindowsError(const TCHAR* Action, uint32 ErrorCode)
        {
            TCHAR SystemMessage[1024] = {};
            FPlatformMisc::GetSystemErrorMessage(SystemMessage, UE_ARRAY_COUNT(SystemMessage), ErrorCode);
            return FString::Printf(TEXT("%s (error %u: %s)"), Action, ErrorCode, SystemMessage);
        }
#elif PLATFORM_LINUX
        // Same "(error N: text)" shape as FormatWindowsError, so callers and tests read one format.
        FString FormatPosixError(const TCHAR* Action, int ErrorCode)
        {
            return FString::Printf(TEXT("%s (error %d: %s)"), Action, ErrorCode,
                UTF8_TO_TCHAR(strerror(ErrorCode)));
        }

        // Mode FUnixPlatformFile::OpenWrite creates files with (0664 before umask).
        constexpr mode_t NewFileMode = S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH;

        // Moves TempPath onto FinalPath only if FinalPath does not exist (the counterpart of
        // MoveFileExW without MOVEFILE_REPLACE_EXISTING). Returns 0 or an errno; EEXIST means
        // the destination exists and nothing moved.
        int PublishWithoutReplacing(const char* TempPath, const char* FinalPath)
        {
            if (renameat2(AT_FDCWD, TempPath, AT_FDCWD, FinalPath, RENAME_NOREPLACE) == 0)
            {
                return 0;
            }
            const int RenameError = errno;
            if (RenameError != EINVAL && RenameError != ENOSYS && RenameError != EOPNOTSUPP)
            {
                return RenameError;
            }

            // The filesystem rejects RENAME_NOREPLACE (e.g. NFS). link(2) refuses an existing
            // destination just as atomically; the temporary name is then dropped. A failed
            // unlink only leaves a second name for bytes that are already published.
            if (link(TempPath, FinalPath) != 0)
            {
                return errno;
            }
            unlink(TempPath);
            return 0;
        }

        // Applies the Windows replacement rules to a regular-file destination before rename(2):
        // a read-only destination is refused (MoveFileExW fails with ERROR_ACCESS_DENIED even
        // for administrators, so a cleared owner-write bit refuses root too), and the
        // replacement keeps the destination's permission bits so a private file (0600) is never
        // republished with the wider default mode. Returns 0 or an errno.
        int PrepareReplacement(const char* TempPath, const char* FinalPath,
            const struct stat& DestinationInfo)
        {
            if (!S_ISREG(DestinationInfo.st_mode))
            {
                return 0;
            }
            if ((DestinationInfo.st_mode & S_IWUSR) == 0
                || (access(FinalPath, W_OK) != 0 && errno == EACCES))
            {
                return EACCES;
            }
            if (chmod(TempPath, DestinationInfo.st_mode & (S_IRWXU | S_IRWXG | S_IRWXO)) != 0)
            {
                return errno;
            }
            return 0;
        }

        // Counterpart of MOVEFILE_WRITE_THROUGH: persist the directory entry a publish created.
        // Best effort, because the new name is already visible and reporting a failed write
        // here would misdescribe the destination.
        void SyncDirectory(const FString& Directory)
        {
            const int DirectoryDescriptor =
                open(TCHAR_TO_UTF8(*Directory), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
            if (DirectoryDescriptor >= 0)
            {
                fsync(DirectoryDescriptor);
                close(DirectoryDescriptor);
            }
        }
#endif

        FStageResult StageBytes(const FString& TempPath, TArrayView<const uint8> Bytes)
        {
#if PLATFORM_WINDOWS
            HANDLE Handle = CreateFileW(
                *TempPath,
                GENERIC_WRITE,
                /*dwShareMode=*/0,
                /*lpSecurityAttributes=*/nullptr,
                CREATE_NEW,
                FILE_ATTRIBUTE_NORMAL,
                /*hTemplateFile=*/nullptr);
            if (Handle == INVALID_HANDLE_VALUE)
            {
                const uint32 ErrorCode = GetLastError();
                if (ErrorCode == ERROR_FILE_EXISTS || ErrorCode == ERROR_ALREADY_EXISTS)
                {
                    return { EStageStatus::Collision, FString() };
                }
                return { EStageStatus::IoError,
                    FormatWindowsError(TEXT("Failed to create atomic-write temporary file"), ErrorCode) };
            }

            bool bSucceeded = true;
            FString Error;
            const uint8* Cursor = Bytes.GetData();
            int32 Remaining = Bytes.Num();
            while (Remaining > 0)
            {
                const DWORD ChunkSize = static_cast<DWORD>(Remaining);
                DWORD Written = 0;
                const BOOL bWriteSucceeded = WriteFile(
                    Handle, Cursor, ChunkSize, &Written, /*lpOverlapped=*/nullptr);
                if (!bWriteSucceeded || Written != ChunkSize)
                {
                    const uint32 ErrorCode = bWriteSucceeded
                        ? ERROR_WRITE_FAULT
                        : GetLastError();
                    Error = FormatWindowsError(TEXT("Failed to write atomic-write temporary file"),
                        ErrorCode);
                    bSucceeded = false;
                    break;
                }
                Cursor += Written;
                Remaining -= static_cast<int32>(Written);
            }

            if (bSucceeded && !FlushFileBuffers(Handle))
            {
                Error = FormatWindowsError(TEXT("Failed to flush atomic-write temporary file"),
                    GetLastError());
                bSucceeded = false;
            }

            if (!CloseHandle(Handle) && bSucceeded)
            {
                Error = FormatWindowsError(TEXT("Failed to close atomic-write temporary file"),
                    GetLastError());
                bSucceeded = false;
            }

            return { bSucceeded ? EStageStatus::Success : EStageStatus::IoError, MoveTemp(Error) };
#elif PLATFORM_LINUX
            const int FileDescriptor = open(TCHAR_TO_UTF8(*TempPath),
                O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, NewFileMode);
            if (FileDescriptor < 0)
            {
                const int ErrorCode = errno;
                if (ErrorCode == EEXIST)
                {
                    return { EStageStatus::Collision, FString() };
                }
                return { EStageStatus::IoError,
                    FormatPosixError(TEXT("Failed to create atomic-write temporary file"), ErrorCode) };
            }

            bool bSucceeded = true;
            FString Error;
            const uint8* Cursor = Bytes.GetData();
            int32 Remaining = Bytes.Num();
            while (Remaining > 0)
            {
                const ssize_t Written = write(FileDescriptor, Cursor, static_cast<size_t>(Remaining));
                if (Written < 0 && errno == EINTR)
                {
                    continue;
                }
                if (Written <= 0)
                {
                    Error = FormatPosixError(TEXT("Failed to write atomic-write temporary file"),
                        Written < 0 ? errno : EIO);
                    bSucceeded = false;
                    break;
                }
                Cursor += Written;
                Remaining -= static_cast<int32>(Written);
            }

            if (bSucceeded && fsync(FileDescriptor) != 0)
            {
                Error = FormatPosixError(TEXT("Failed to flush atomic-write temporary file"), errno);
                bSucceeded = false;
            }

            // Linux releases the descriptor even when close(2) reports EINTR, and the bytes were
            // already flushed above, so only other errors count.
            if (close(FileDescriptor) != 0 && errno != EINTR && bSucceeded)
            {
                Error = FormatPosixError(TEXT("Failed to close atomic-write temporary file"), errno);
                bSucceeded = false;
            }

            return { bSucceeded ? EStageStatus::Success : EStageStatus::IoError, MoveTemp(Error) };
#else
            return { EStageStatus::IoError,
                TEXT("Atomic file publication is supported only on Win64 and Linux.") };
#endif
        }

        FResult PublishTempFile(const FString& TempPath, const FString& FinalPath,
            EExistingFilePolicy ExistingFilePolicy, const FOperations& Operations)
        {
#if PLATFORM_WINDOWS
            for (int32 Attempt = 0; Attempt < MaxPublishAttempts; ++Attempt)
            {
                if (MoveFileExW(*TempPath, *FinalPath, MOVEFILE_WRITE_THROUGH))
                {
                    FResult Result;
                    Result.Status = EStatus::Success;
                    Result.bReplaced = false;
                    return Result;
                }

                const uint32 CreateError = GetLastError();
                const bool bDestinationExists = CreateError == ERROR_FILE_EXISTS
                    || CreateError == ERROR_ALREADY_EXISTS;
                if (!bDestinationExists)
                {
                    FResult Result;
                    Result.Status = EStatus::IoError;
                    Result.Error = FormatWindowsError(
                        TEXT("Failed to publish new atomic-write file"), CreateError);
                    return Result;
                }

                if (ExistingFilePolicy == EExistingFilePolicy::FailIfExists)
                {
                    FResult Result;
                    Result.Status = EStatus::AlreadyExists;
                    Result.Error = FString::Printf(TEXT("Destination already exists: %s"), *FinalPath);
                    return Result;
                }

                TFunction<void()> ReleasePreReplace;
                if (Operations.PreReplace)
                {
                    Operations.PreReplace(TempPath, ReleasePreReplace);
                }

                const DWORD DestinationAttributes = GetFileAttributesW(*FinalPath);
                if (DestinationAttributes == INVALID_FILE_ATTRIBUTES)
                {
                    const uint32 StatError = GetLastError();
                    if (ReleasePreReplace)
                    {
                        ReleasePreReplace();
                    }
                    if (StatError == ERROR_FILE_NOT_FOUND)
                    {
                        continue;
                    }

                    FResult Result;
                    Result.Status = EStatus::IoError;
                    Result.Error = FormatWindowsError(
                        TEXT("Failed to re-stat atomic-write destination"), StatError);
                    return Result;
                }

                const BOOL bReplaceSucceeded = MoveFileExW(*TempPath, *FinalPath,
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
                const uint32 ReplaceError = bReplaceSucceeded ? ERROR_SUCCESS : GetLastError();
                if (ReleasePreReplace)
                {
                    ReleasePreReplace();
                }

                FResult Result;
                if (bReplaceSucceeded)
                {
                    Result.Status = EStatus::Success;
                    Result.bReplaced = true;
                    return Result;
                }

                Result.Status = EStatus::IoError;
                Result.Error = FormatWindowsError(
                    TEXT("Failed to replace existing atomic-write file"), ReplaceError);
                return Result;
            }

            FResult Result;
            Result.Status = EStatus::IoError;
            Result.Error = FString::Printf(
                TEXT("Destination disappeared repeatedly during atomic replacement: %s"),
                *FinalPath);
            return Result;
#elif PLATFORM_LINUX
            const FTCHARToUTF8 TempPathUtf8(*TempPath);
            const FTCHARToUTF8 FinalPathUtf8(*FinalPath);
            for (int32 Attempt = 0; Attempt < MaxPublishAttempts; ++Attempt)
            {
                const int CreateError = PublishWithoutReplacing(TempPathUtf8.Get(), FinalPathUtf8.Get());
                if (CreateError == 0)
                {
                    SyncDirectory(FPaths::GetPath(FinalPath));
                    FResult Result;
                    Result.Status = EStatus::Success;
                    Result.bReplaced = false;
                    return Result;
                }

                if (CreateError != EEXIST)
                {
                    FResult Result;
                    Result.Status = EStatus::IoError;
                    Result.Error = FormatPosixError(
                        TEXT("Failed to publish new atomic-write file"), CreateError);
                    return Result;
                }

                if (ExistingFilePolicy == EExistingFilePolicy::FailIfExists)
                {
                    FResult Result;
                    Result.Status = EStatus::AlreadyExists;
                    Result.Error = FString::Printf(TEXT("Destination already exists: %s"), *FinalPath);
                    return Result;
                }

                TFunction<void()> ReleasePreReplace;
                if (Operations.PreReplace)
                {
                    Operations.PreReplace(TempPath, ReleasePreReplace);
                }

                // lstat, like GetFileAttributesW: a symlink destination is replaced, not followed.
                struct stat DestinationInfo;
                if (lstat(FinalPathUtf8.Get(), &DestinationInfo) != 0)
                {
                    const int StatError = errno;
                    if (ReleasePreReplace)
                    {
                        ReleasePreReplace();
                    }
                    if (StatError == ENOENT)
                    {
                        continue;
                    }

                    FResult Result;
                    Result.Status = EStatus::IoError;
                    Result.Error = FormatPosixError(
                        TEXT("Failed to re-stat atomic-write destination"), StatError);
                    return Result;
                }

                int ReplaceError = PrepareReplacement(
                    TempPathUtf8.Get(), FinalPathUtf8.Get(), DestinationInfo);
                if (ReplaceError == 0 && rename(TempPathUtf8.Get(), FinalPathUtf8.Get()) != 0)
                {
                    ReplaceError = errno;
                }
                if (ReleasePreReplace)
                {
                    ReleasePreReplace();
                }

                FResult Result;
                if (ReplaceError == 0)
                {
                    SyncDirectory(FPaths::GetPath(FinalPath));
                    Result.Status = EStatus::Success;
                    Result.bReplaced = true;
                    return Result;
                }

                Result.Status = EStatus::IoError;
                Result.Error = FormatPosixError(
                    TEXT("Failed to replace existing atomic-write file"), ReplaceError);
                return Result;
            }

            FResult Result;
            Result.Status = EStatus::IoError;
            Result.Error = FString::Printf(
                TEXT("Destination disappeared repeatedly during atomic replacement: %s"),
                *FinalPath);
            return Result;
#else
            FResult Result;
            Result.Status = EStatus::IoError;
            Result.Error = TEXT("Atomic file publication is supported only on Win64 and Linux.");
            return Result;
#endif
        }

        bool VerifyStagedBytes(const FString& TempPath, TArrayView<const uint8> ExpectedBytes,
            FString& OutError)
        {
            IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
            TUniquePtr<IFileHandle> Reader(PlatformFile.OpenRead(*TempPath));
            if (!Reader)
            {
                OutError = FString::Printf(TEXT("Failed to read staged atomic-write file: %s"), *TempPath);
                return false;
            }

            const int64 ActualSize = Reader->Size();
            if (ActualSize != ExpectedBytes.Num())
            {
                OutError = FString::Printf(
                    TEXT("Staged atomic-write verification failed for %s (expected %d bytes, read %lld)."),
                    *TempPath, ExpectedBytes.Num(), ActualSize);
                return false;
            }

            TArray<uint8> Buffer;
            Buffer.SetNumUninitialized(VerificationChunkSize);
            int32 Offset = 0;
            while (Offset < ExpectedBytes.Num())
            {
                const int32 ChunkSize = FMath::Min(
                    VerificationChunkSize, ExpectedBytes.Num() - Offset);
                if (!Reader->Read(Buffer.GetData(), ChunkSize))
                {
                    OutError = FString::Printf(
                        TEXT("Failed to read staged atomic-write file at byte %d: %s"),
                        Offset, *TempPath);
                    return false;
                }
                if (FMemory::Memcmp(Buffer.GetData(), ExpectedBytes.GetData() + Offset,
                        ChunkSize) != 0)
                {
                    OutError = FString::Printf(
                        TEXT("Staged atomic-write byte verification failed at byte %d: %s"),
                        Offset, *TempPath);
                    return false;
                }
                Offset += ChunkSize;
            }
            return true;
        }

        void DeleteTempFile(const FString& TempPath, FString& InOutError)
        {
            IFileManager& FileManager = IFileManager::Get();
            if (!FileManager.Delete(*TempPath, /*RequireExists=*/false, /*EvenReadOnly=*/false,
                    /*Quiet=*/true)
                && FileManager.FileExists(*TempPath))
            {
                if (!InOutError.IsEmpty())
                {
                    InOutError += TEXT(" ");
                }
                InOutError += FString::Printf(TEXT("Failed to remove temporary file: %s"), *TempPath);
            }
        }

        FResult WriteBytesImpl(const FString& InFinalPath, TArrayView<const uint8> Bytes,
            EExistingFilePolicy ExistingFilePolicy, const FOperations& Operations)
        {
            FResult Result;
            if (InFinalPath.IsEmpty())
            {
                Result.Error = TEXT("Atomic-write destination path is empty.");
                return Result;
            }

            FString FinalPath = FPaths::ConvertRelativePathToFull(InFinalPath);
            FPaths::NormalizeFilename(FinalPath);
            const FString Directory = FPaths::GetPath(FinalPath);

            if (Directory.IsEmpty())
            {
                Result.Error = TEXT("Atomic-write destination path has no parent directory.");
                return Result;
            }

            IFileManager& FileManager = IFileManager::Get();
            if (!FileManager.DirectoryExists(*Directory))
            {
#if PLATFORM_WINDOWS
                const bool bCreated = FileManager.MakeDirectory(*Directory, /*Tree=*/true);
                if (!bCreated)
                {
                    Result.Error = FormatWindowsError(
                        TEXT("Failed to create destination directory"), GetLastError());
                    return Result;
                }
#else
                if (!FileManager.MakeDirectory(*Directory, /*Tree=*/true))
                {
                    Result.Error = FString::Printf(TEXT("Failed to create destination directory: %s"),
                        *Directory);
                    return Result;
                }
#endif
            }

            for (int32 Attempt = 0; Attempt < MaxTempNameAttempts; ++Attempt)
            {
                const FString TempPath = FinalPath + TEXT(".")
                    + FGuid::NewGuid().ToString(EGuidFormats::Digits) + TEXT(".tmp");

                const FStageResult StageResult = Operations.Stage
                    ? Operations.Stage(TempPath, Bytes)
                    : StageBytes(TempPath, Bytes);
                if (StageResult.Status == EStageStatus::Collision)
                {
                    continue;
                }
                if (StageResult.Status != EStageStatus::Success)
                {
                    Result.Error = StageResult.Error.IsEmpty()
                        ? FString::Printf(TEXT("Failed to stage atomic-write file: %s"), *TempPath)
                        : StageResult.Error;
                    DeleteTempFile(TempPath, Result.Error);
                    return Result;
                }

                if (!VerifyStagedBytes(TempPath, Bytes, Result.Error))
                {
                    DeleteTempFile(TempPath, Result.Error);
                    return Result;
                }

                Result = PublishTempFile(TempPath, FinalPath, ExistingFilePolicy, Operations);
                if (!Result.IsSuccess())
                {
                    DeleteTempFile(TempPath, Result.Error);
                }
                return Result;
            }

            Result.Error = FString::Printf(
                TEXT("Could not allocate a unique atomic-write temporary file beside: %s"), *FinalPath);
            return Result;
        }
    }

#if WITH_DEV_AUTOMATION_TESTS
    FResult WriteBytesWithOperationsForTests(const FString& FinalPath,
        TArrayView<const uint8> Bytes, EExistingFilePolicy ExistingFilePolicy,
        const FOperations& Operations)
    {
        return WriteBytesImpl(FinalPath, Bytes, ExistingFilePolicy, Operations);
    }
#endif
}

AtomicFileWriter::FResult AtomicFileWriter::WriteBytes(const FString& FinalPath,
    TArrayView<const uint8> Bytes, EExistingFilePolicy ExistingFilePolicy)
{
    return Private::WriteBytesImpl(FinalPath, Bytes, ExistingFilePolicy, Private::FOperations());
}

AtomicFileWriter::FResult AtomicFileWriter::WriteUtf8(const FString& FinalPath, FStringView Text,
    EExistingFilePolicy ExistingFilePolicy)
{
    const TCHAR* TextData = Text.Len() == 0 ? TEXT("") : Text.GetData();
    const FTCHARToUTF8 Utf8(TextData, Text.Len());
    return WriteBytes(FinalPath,
        TArrayView<const uint8>(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length()),
        ExistingFilePolicy);
}

#if PLATFORM_WINDOWS
#include "Windows/HideWindowsPlatformTypes.h"
#endif
