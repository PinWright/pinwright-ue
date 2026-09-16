// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "Utils/AtomicFileWriter.h"
#include "Templates/Function.h"

namespace AtomicFileWriter::Private
{
    enum class EStageStatus : uint8
    {
        Success,
        Collision,
        IoError
    };

    struct FStageResult
    {
        EStageStatus Status = EStageStatus::IoError;
        FString Error;
    };

    struct FOperations
    {
        TFunction<FStageResult(const FString&, TArrayView<const uint8>)> Stage;

        // Runs only after a non-replacing publish reports a destination collision,
        // before the destination re-stat and real replacement primitive. The hook may
        // provide a release callback, which runs before retry or temporary-file cleanup.
        TFunction<void(const FString&, TFunction<void()>&)> PreReplace;
    };

#if WITH_DEV_AUTOMATION_TESTS
    // Test-only entry point for injecting one operation into the production state machine.
    FResult WriteBytesWithOperationsForTests(
        const FString& FinalPath,
        TArrayView<const uint8> Bytes,
        EExistingFilePolicy ExistingFilePolicy,
        const FOperations& Operations);
#endif
}
