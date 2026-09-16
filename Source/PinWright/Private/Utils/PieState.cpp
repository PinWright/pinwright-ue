// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Utils/PieState.h"

#include "CoreGlobals.h"
#include "Containers/Ticker.h"
#include "Editor.h"
#include "HAL/PlatformTime.h"

namespace PinWrightPieState
{

namespace
{
#if WITH_DEV_AUTOMATION_TESTS
    TWeakPtr<FPieLifecycleWaitControl> GLastPieLifecycleWaitControl;
#endif

    bool IsTerminalWaitStatus(const EPieLifecycleWaitStatus Status)
    {
        return Status != EPieLifecycleWaitStatus::Pending;
    }

    FPieLifecycleWaitResult MakeWaitResult(
        const FPieLifecycleState& State,
        const EPieLifecycleWaitStatus Status)
    {
        return {
            State.bPieActive,
            State.bSessionInProgress,
            State.bStartRequestQueued,
            Status == EPieLifecycleWaitStatus::TimedOut,
            Status == EPieLifecycleWaitStatus::Cancelled,
        };
    }
}

void FPieLifecycleWaitControl::SetTickerHandle(
    FTSTicker::FDelegateHandle InTickerHandle)
{
    check(!TickerHandle.IsValid());
    TickerHandle = MoveTemp(InTickerHandle);
}

bool FPieLifecycleWaitControl::Cancel()
{
    if (bTerminal)
    {
        return false;
    }

    bTerminal = true;
    if (TickerHandle.IsValid())
    {
        FTSTicker::GetCoreTicker().RemoveTicker(TickerHandle);
        TickerHandle.Reset();
    }
    return true;
}

bool FPieLifecycleOperationOwner::HasActiveOperation() const
{
    return ActiveOperation.IsValid();
}

TSharedPtr<FPieLifecycleOperation> FPieLifecycleOperationOwner::GetActiveOperation() const
{
    return ActiveOperation;
}

TSharedPtr<FPieLifecycleOperation> FPieLifecycleOperationOwner::TryBegin(
    const EPieLifecycleOperationType Type)
{
    if (ActiveOperation.IsValid())
    {
        return nullptr;
    }

    TSharedPtr<FPieLifecycleOperation> Operation = MakeShared<FPieLifecycleOperation>();
    Operation->Generation = ++LastGeneration;
    Operation->Type = Type;
    ActiveOperation = Operation;
    return Operation;
}

void FPieLifecycleOperationOwner::CancelActiveOperation()
{
    if (ActiveOperation.IsValid())
    {
        ActiveOperation->bCancelled = true;
        ActiveOperation.Reset();
    }
}

bool FPieLifecycleOperationOwner::IsCurrent(
    const TSharedRef<FPieLifecycleOperation>& Operation) const
{
    return ActiveOperation.IsValid()
        && ActiveOperation.Get() == &Operation.Get()
        && ActiveOperation->Generation == Operation->Generation
        && !Operation->bCancelled;
}

void FPieLifecycleOperationOwner::Complete(
    const TSharedRef<FPieLifecycleOperation>& Operation)
{
    if (IsCurrent(Operation))
    {
        ActiveOperation.Reset();
    }
}

bool IsPlayInEditorActive()
{
    return GEditor && (GEditor->PlayWorld != nullptr || GIsPlayInEditorWorld);
}

FPieLifecycleState CaptureEditorPieLifecycleState()
{
    if (!GEditor)
    {
        return {};
    }

    return {
        GEditor->PlayWorld != nullptr,
        GEditor->IsPlaySessionInProgress(),
        GEditor->IsPlaySessionRequestQueued(),
    };
}

EPieLifecycleWaitStatus EvaluatePieLifecycleWait(
    const EPieLifecycleWaitTarget Target,
    const FPieLifecycleState& State,
    const bool bDeadlineReached,
    const bool bCancelled)
{
    if (bCancelled)
    {
        return EPieLifecycleWaitStatus::Cancelled;
    }

    const bool bReached = Target == EPieLifecycleWaitTarget::PlayWorldActive
        ? State.bPieActive
        : !State.bPieActive && !State.bSessionInProgress;
    if (bReached)
    {
        return EPieLifecycleWaitStatus::Reached;
    }
    return bDeadlineReached
        ? EPieLifecycleWaitStatus::TimedOut
        : EPieLifecycleWaitStatus::Pending;
}

EPieStopAction EvaluatePieStopAction(const FPieLifecycleState& State)
{
    if (State.bPieActive)
    {
        return EPieStopAction::RequestEndPlay;
    }
    if (State.bStartRequestQueued)
    {
        return EPieStopAction::CancelQueuedStartRequest;
    }
    if (State.bSessionInProgress)
    {
        return EPieStopAction::WaitForTransition;
    }
    return EPieStopAction::None;
}

TSharedRef<FPieLifecycleWaitControl> WaitForPieLifecycleState(
    const EPieLifecycleWaitTarget Target,
    const double TimeoutSeconds,
    FPieLifecycleWaitComplete Completion,
    FPieLifecycleStateProbe StateProbe,
    FPieLifecycleCancellationProbe CancellationProbe)
{
    const TSharedRef<FPieLifecycleWaitControl> Control =
        MakeShared<FPieLifecycleWaitControl>();
#if WITH_DEV_AUTOMATION_TESTS
    GLastPieLifecycleWaitControl = Control.ToWeakPtr();
#endif
    if (!Completion)
    {
        Control->Cancel();
        return Control;
    }
    if (!StateProbe)
    {
        StateProbe = CaptureEditorPieLifecycleState;
    }

    const double Deadline = FPlatformTime::Seconds() + FMath::Max(0.0, TimeoutSeconds);
    const FPieLifecycleState State = StateProbe();
    const EPieLifecycleWaitStatus Status = EvaluatePieLifecycleWait(
        Target,
        State,
        FPlatformTime::Seconds() >= Deadline,
        CancellationProbe && CancellationProbe());
    if (IsTerminalWaitStatus(Status))
    {
        Control->Cancel();
        Completion(MakeWaitResult(State, Status));
        return Control;
    }

    Control->SetTickerHandle(FTSTicker::GetCoreTicker().AddTicker(
        FTickerDelegate::CreateLambda(
            [Control, Target, Deadline, Completion = MoveTemp(Completion),
             StateProbe = MoveTemp(StateProbe),
             CancellationProbe = MoveTemp(CancellationProbe)](float /*DeltaTime*/) mutable -> bool
            {
                const FPieLifecycleState CurrentState = StateProbe();
                const EPieLifecycleWaitStatus CurrentStatus = EvaluatePieLifecycleWait(
                    Target,
                    CurrentState,
                    FPlatformTime::Seconds() >= Deadline,
                    CancellationProbe && CancellationProbe());
                if (IsTerminalWaitStatus(CurrentStatus))
                {
                    Control->Cancel();
                    Completion(MakeWaitResult(CurrentState, CurrentStatus));
                    return false;
                }
                return true;
            }),
        0.05f));
    return Control;
}

#if WITH_DEV_AUTOMATION_TESTS
TSharedPtr<FPieLifecycleWaitControl> GetLastPieLifecycleWaitControlForTesting()
{
    return GLastPieLifecycleWaitControl.Pin();
}
#endif

}
