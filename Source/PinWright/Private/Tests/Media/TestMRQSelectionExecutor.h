// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "MoviePipelineExecutor.h"
#include "MoviePipelineQueue.h"
#if WITH_DEV_AUTOMATION_TESTS
    #include "Handlers/MRQ/MRQHandlerTestHooks.h"
#endif
#include "TestMRQSelectionExecutor.generated.h"

// Test-only executor used by MRQ handler tests. It records the queue handed to
// Execute_Implementation and completes synchronously, so selection behavior is
// exercised without starting PIE or rendering any frames.
UCLASS()
class UTestMRQSelectionExecutor : public UMoviePipelineExecutorBase
{
    GENERATED_BODY()

public:
    inline static TArray<FString> ObservedJobNames;
    inline static TArray<bool> ObservedJobEnabled;

    static void ResetObservation()
    {
        ObservedJobNames.Reset();
        ObservedJobEnabled.Reset();
    }

protected:
    virtual void Execute_Implementation(UMoviePipelineQueue* InPipelineQueue) override
    {
        ObservedJobNames.Reset();
        ObservedJobEnabled.Reset();
        if (InPipelineQueue)
        {
            for (UMoviePipelineExecutorJob* Job : InPipelineQueue->GetJobs())
            {
                ObservedJobNames.Add(Job ? Job->JobName : FString());
                ObservedJobEnabled.Add(Job && Job->IsEnabled());
            }
        }
#if WITH_DEV_AUTOMATION_TESTS
        if (const PinWrightMRQHandlerTestHooks::FInjectedExecutorResult* Injected =
            PinWrightMRQHandlerTestHooks::ActiveExecutorResult())
        {
            if (Injected->bExecutorFailed)
            {
                const FString FailureMessage = Injected->ExecutorFailureMessage.IsEmpty()
                    ? FString(TEXT("Injected MRQ executor failure"))
                    : Injected->ExecutorFailureMessage;
                SetStatusMessage(FailureMessage);
                OnExecutorErroredImpl(nullptr, Injected->bExecutorFailureFatal,
                    FText::FromString(FailureMessage));
                return;
            }
        }
#endif
        OnExecutorFinishedImpl();
    }

    virtual bool IsRendering_Implementation() const override
    {
        return false;
    }

    virtual void CancelCurrentJob_Implementation() override {}
    virtual void CancelAllJobs_Implementation() override {}
};
