// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#if WITH_DEV_AUTOMATION_TESTS

#include "Handlers/MRQ/MRQArtifactReport.h"

namespace PinWrightMRQHandlerTestHooks
{
    // Narrow seam for terminal-result behavior. The real executor still starts and finishes, but
    // tests can supply the output paths its per-job callback would have reported. This keeps the
    // black-frame test off PIE while sending the fixture through the production decoder, artifact
    // report, completion classifier and job registry.
    struct FInjectedExecutorResult
    {
        TArray<PinWrightMRQ::FRenderedFile> Files;
        PinWrightMRQ::FEncodeContext EncodeContext;
        FString JobName;
        bool bJobSucceeded = true;
        bool bExecutorFailed = false;
        bool bExecutorFailureFatal = true;
        FString ExecutorFailureMessage;
    };

    inline const FInjectedExecutorResult*& ActiveExecutorResult()
    {
        static const FInjectedExecutorResult* Result = nullptr;
        return Result;
    }

    class FScopedInjectedExecutorResult final
    {
    public:
        explicit FScopedInjectedExecutorResult(const FInjectedExecutorResult& Result)
            : Previous(ActiveExecutorResult())
        {
            ActiveExecutorResult() = &Result;
        }

        ~FScopedInjectedExecutorResult()
        {
            ActiveExecutorResult() = Previous;
        }

        FScopedInjectedExecutorResult(const FScopedInjectedExecutorResult&) = delete;
        FScopedInjectedExecutorResult& operator=(const FScopedInjectedExecutorResult&) = delete;

    private:
        const FInjectedExecutorResult* Previous = nullptr;
    };
}

#endif
