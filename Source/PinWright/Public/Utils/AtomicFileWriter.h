// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"

namespace AtomicFileWriter
{
    enum class EExistingFilePolicy : uint8
    {
        FailIfExists,
        ReplaceExisting
    };

    enum class EStatus : uint8
    {
        Success,
        AlreadyExists,
        IoError
    };

    struct FResult
    {
        EStatus Status = EStatus::IoError;
        bool bReplaced = false;
        FString Error;

        bool IsSuccess() const { return Status == EStatus::Success; }
    };

    // Writes raw bytes through a unique same-directory temporary file, verifies the
    // staged bytes, then publishes without pre-deleting an existing destination.
    // A read-only destination is never replaced (IoError). On Linux a replacement keeps
    // the destination's permission bits.
    PINWRIGHT_API FResult WriteBytes(
        const FString& FinalPath,
        TArrayView<const uint8> Bytes,
        EExistingFilePolicy ExistingFilePolicy);

    // UTF-8 without BOM. Line endings are preserved exactly as supplied.
    PINWRIGHT_API FResult WriteUtf8(
        const FString& FinalPath,
        FStringView Text,
        EExistingFilePolicy ExistingFilePolicy);
}
