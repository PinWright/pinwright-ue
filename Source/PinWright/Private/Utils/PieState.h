// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "Containers/Ticker.h"
#include "CoreMinimal.h"

namespace PinWrightPieState
{
    struct FPieLifecycleState
    {
        bool bPieActive = false;
        bool bSessionInProgress = false;
        bool bStartRequestQueued = false;
    };

    struct FPieLifecycleWaitResult
    {
        bool bPieActive = false;
        bool bSessionInProgress = false;
        bool bStartRequestQueued = false;
        bool bTimedOut = false;
        bool bCancelled = false;
    };

    enum class EPieLifecycleWaitTarget : uint8
    {
        PlayWorldActive,
        SessionInactive,
    };

    enum class EPieLifecycleWaitStatus : uint8
    {
        Pending,
        Reached,
        TimedOut,
        Cancelled,
    };

    // Cancel is valid only while Unreal still owns a queued request. Session-info-only state
    // must survive until deferred engine callbacks either create PlayWorld or end the session.
    enum class EPieStopAction : uint8
    {
        None,
        CancelQueuedStartRequest,
        WaitForTransition,
        RequestEndPlay,
    };

    enum class EPieLifecycleOperationType : uint8
    {
        Play,
        Stop,
        Cleanup,
    };

    struct FPieLifecycleOperation
    {
        uint64 Generation = 0;
        EPieLifecycleOperationType Type = EPieLifecycleOperationType::Play;
        bool bCancelled = false;
    };

    // Small editor-thread-local ownership gate. A replacement operation gets a new generation,
    // so a cancelled waiter cannot observe a later operation's PlayWorld as its own success.
    class PINWRIGHT_API FPieLifecycleOperationOwner
    {
    public:
        bool HasActiveOperation() const;
        TSharedPtr<FPieLifecycleOperation> GetActiveOperation() const;
        TSharedPtr<FPieLifecycleOperation> TryBegin(EPieLifecycleOperationType Type);
        void CancelActiveOperation();
        bool IsCurrent(const TSharedRef<FPieLifecycleOperation>& Operation) const;
        void Complete(const TSharedRef<FPieLifecycleOperation>& Operation);

    private:
        uint64 LastGeneration = 0;
        TSharedPtr<FPieLifecycleOperation> ActiveOperation;
    };

    using FPieLifecycleStateProbe = TFunction<FPieLifecycleState()>;
    using FPieLifecycleCancellationProbe = TFunction<bool()>;
    using FPieLifecycleWaitComplete = TFunction<void(const FPieLifecycleWaitResult&)>;

    // Owns one lifecycle wait ticker. Cancel removes and invalidates the ticker
    // without invoking Completion, allowing a higher-level owner to decide the
    // terminal response and release its matching lifecycle generation.
    class PINWRIGHT_API FPieLifecycleWaitControl
    {
    public:
        bool Cancel();
        bool IsTickerValid() const { return TickerHandle.IsValid(); }

    private:
        friend PINWRIGHT_API TSharedRef<FPieLifecycleWaitControl> WaitForPieLifecycleState(
            EPieLifecycleWaitTarget Target,
            double TimeoutSeconds,
            FPieLifecycleWaitComplete Completion,
            FPieLifecycleStateProbe StateProbe,
            FPieLifecycleCancellationProbe CancellationProbe);

        void SetTickerHandle(FTSTicker::FDelegateHandle InTickerHandle);

        FTSTicker::FDelegateHandle TickerHandle;
        bool bTerminal = false;
    };

    // Mirrors EditorScriptingHelpers::CheckIfInEditorAndPIE's play-mode predicate.
    // Use this before an editor API that silently returns a failure sentinel during PIE.
    PINWRIGHT_API bool IsPlayInEditorActive();

    // Read both lifecycle signals Unreal exposes: PlayWorld is the play-success contract,
    // while IsPlaySessionInProgress also covers queued and session-info-only transitions.
    PINWRIGHT_API FPieLifecycleState CaptureEditorPieLifecycleState();

    // Pure decision seams used by both the ticker and editor-free lifecycle tests. Cancellation
    // wins over a reached target so a stale waiter cannot claim a newer session.
    PINWRIGHT_API EPieLifecycleWaitStatus EvaluatePieLifecycleWait(
        EPieLifecycleWaitTarget Target,
        const FPieLifecycleState& State,
        bool bDeadlineReached,
        bool bCancelled);
    PINWRIGHT_API EPieStopAction EvaluatePieStopAction(const FPieLifecycleState& State);

    // Play/stop requests are applied on later editor ticks. PlayWorldActive completes as soon
    // as the primary PIE world exists. SessionInactive completes only after Unreal's
    // authoritative session predicate is false and PlayWorld is gone. The probes are injectable
    // for editor-free tests.
    PINWRIGHT_API TSharedRef<FPieLifecycleWaitControl> WaitForPieLifecycleState(
        EPieLifecycleWaitTarget Target,
        double TimeoutSeconds,
        FPieLifecycleWaitComplete Completion,
        FPieLifecycleStateProbe StateProbe = FPieLifecycleStateProbe(),
        FPieLifecycleCancellationProbe CancellationProbe = FPieLifecycleCancellationProbe());

#if WITH_DEV_AUTOMATION_TESTS
    PINWRIGHT_API TSharedPtr<FPieLifecycleWaitControl>
        GetLastPieLifecycleWaitControlForTesting();
#endif
}
