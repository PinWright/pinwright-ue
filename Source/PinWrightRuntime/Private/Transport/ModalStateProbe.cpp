// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Transport/ModalStateProbe.h"

#include "Handlers/ErrorCodes.h"

#include "Framework/Application/SlateApplication.h"
#include "HAL/CriticalSection.h"
#include "HAL/PlatformTime.h"
#include "Logging/LogMacros.h"
#include "Widgets/SWindow.h"

#include <atomic>

// The on-disk trace for a modal block. Its own category, not LogPinWright, so a
// forensic pass can grep one name and get every modal episode with nothing else
// in the way. See OnModalLoopTick for why the trace has to exist at all.
DEFINE_LOG_CATEGORY_STATIC(LogPinWrightModal, Log, All);

namespace ModalStateProbe
{
namespace
{
    // Written by the game thread (modal loop + subsystem tick), read by the
    // socket I/O thread. Contention is nil: the game thread writes at ~60 Hz
    // only while blocked, the I/O thread reads only when a request arrives, and
    // the lock is released between modal-loop iterations.
    std::atomic<bool> GBlocked { false };
    std::atomic<double> GBlockStartSeconds { 0.0 };

    FCriticalSection GTitleMutex;
    FString GModalTitle;

    // Absolute FPlatformTime::Seconds() of the last proven-healthy game-thread
    // tick. 0.0 means "never stamped" - see GameThreadStalledSeconds().
    std::atomic<double> GLastAliveSeconds { 0.0 };

    // The RPC currently on the game thread. All three fields under ONE lock
    // rather than a mix of atomics: they are only ever meaningful together, and
    // splitting them would let a reader pair a live method with a cleared start
    // time. The game thread holds this lock for the duration of two FString
    // copies, so a wedge can never be holding it and the I/O-thread reader can
    // never be starved by one. 0.0 start means idle.
    FCriticalSection GInFlightMutex;
    FString GInFlightMethod;
    FString GInFlightRequestId;
    double GInFlightStartSeconds = 0.0;

    FDelegateHandle GModalLoopTickHandle;

    void SetTitle(const FString& InTitle)
    {
        FScopeLock Lock(&GTitleMutex);
        GModalTitle = InTitle;
    }

    // Best-effort: a cancelled/absent modal leaves this empty and the caller
    // omits the field rather than inventing a name.
    FString ReadActiveModalTitle()
    {
        if (!FSlateApplication::IsInitialized())
        {
            return FString();
        }
        const TSharedPtr<SWindow> ActiveModal = FSlateApplication::Get().GetActiveModalWindow();
        return ActiveModal.IsValid() ? ActiveModal->GetTitle().ToString() : FString();
    }
}

void Register()
{
    if (GModalLoopTickHandle.IsValid() || !FSlateApplication::IsInitialized())
    {
        return;
    }
    GModalLoopTickHandle =
        FSlateApplication::Get().GetOnModalLoopTickEvent().AddStatic(&ModalStateProbe::OnModalLoopTick);
}

void Unregister()
{
    if (!GModalLoopTickHandle.IsValid())
    {
        return;
    }
    if (FSlateApplication::IsInitialized())
    {
        FSlateApplication::Get().GetOnModalLoopTickEvent().Remove(GModalLoopTickHandle);
    }
    GModalLoopTickHandle.Reset();
}

void OnModalLoopTick(float DeltaTime)
{
    // Read before latching so the arming log below can name the window in the
    // same pass rather than re-entering Slate for it.
    const FString Title = ReadActiveModalTitle();

    const bool bWasBlocked = GBlocked.load(std::memory_order_acquire);
    if (!bWasBlocked)
    {
        // Publish the start time BEFORE the flag so an I/O-thread reader that
        // sees blocked=true never computes an elapsed time against a stale zero.
        GBlockStartSeconds.store(FPlatformTime::Seconds(), std::memory_order_release);
        GBlocked.store(true, std::memory_order_release);
    }

    // Refresh every tick, not just the first: a modal can be replaced by
    // another without the core ticker ever running in between.
    SetTitle(Title);

    if (!bWasBlocked)
    {
        // ON-DISK TRACE, one line per episode.
        //
        // EDITOR_BLOCKED_ON_MODAL is a WIRE-ONLY field: both emit sites
        // (McpRequestCore.cpp BuildPingResult / BuildEditorNotReadyToolResult)
        // write it into an HTTP response body, and neither is a UE_LOG - so
        // until this line existed, the one failure class that no amount of
        // polling can clear left Saved/Logs/ completely silent. Nothing on disk
        // could confirm OR refute a claim about how often it fires, which is
        // precisely how a fabricated "779 occurrences in the log corpus" figure
        // survived two triage passes. The condition is now falsifiable by grep.
        //
        // Logged on the latch EDGE, not on the ~60 Hz modal-loop tick, so a
        // half-hour wedge costs exactly one line and one more when it clears.
        UE_LOG(LogPinWrightModal, Warning,
               TEXT("Modal window owns the game thread%s%s - every RPC now answers %s (non-retryable) until it is dismissed."),
               Title.IsEmpty() ? TEXT("") : TEXT(": "),
               Title.IsEmpty() ? TEXT(" (title unavailable)") : *Title,
               ErrorCodes::ERR_EDITOR_BLOCKED_ON_MODAL);
    }
}

void NoteGameThreadAlive()
{
    // The heartbeat is stamped FIRST and unconditionally. It must not sit behind
    // the blocked early-out below: the healthy path is the only path that runs
    // every tick, and it is precisely the path that has to keep the stamp fresh.
    GLastAliveSeconds.store(FPlatformTime::Seconds(), std::memory_order_release);

    // Cheap early-out: the common case is one relaxed atomic load per tick.
    if (!GBlocked.load(std::memory_order_acquire))
    {
        return;
    }
    // Measure before clearing: BlockedSeconds() returns 0 once the flag is down.
    const double HeldSeconds = BlockedSeconds();
    const FString Title = GetModalTitle();

    GBlocked.store(false, std::memory_order_release);
    GBlockStartSeconds.store(0.0, std::memory_order_release);
    SetTitle(FString());

    // The closing half of the episode trace. A start line with no end line means
    // the editor never came back, which is a different diagnosis from a dialog
    // that was dismissed - so the pair is what makes the log readable, not the
    // start alone.
    UE_LOG(LogPinWrightModal, Warning,
           TEXT("Modal block cleared after %.1f s%s%s - the game thread is running again."),
           HeldSeconds,
           Title.IsEmpty() ? TEXT("") : TEXT(": "),
           Title.IsEmpty() ? TEXT("") : *Title);
}

void NoteRpcDispatchBegin(const FString& RequestId, const FString& Method)
{
    FScopeLock Lock(&GInFlightMutex);
    GInFlightMethod = Method;
    GInFlightRequestId = RequestId;
    GInFlightStartSeconds = FPlatformTime::Seconds();
}

void NoteRpcDispatchEnd()
{
    FScopeLock Lock(&GInFlightMutex);
    GInFlightMethod.Reset();
    GInFlightRequestId.Reset();
    GInFlightStartSeconds = 0.0;
}

bool IsBlocked()
{
    return GBlocked.load(std::memory_order_acquire);
}

double BlockedSeconds()
{
    if (!GBlocked.load(std::memory_order_acquire))
    {
        return 0.0;
    }
    const double Start = GBlockStartSeconds.load(std::memory_order_acquire);
    if (Start <= 0.0)
    {
        return 0.0;
    }
    return FMath::Max(0.0, FPlatformTime::Seconds() - Start);
}

FString GetModalTitle()
{
    FScopeLock Lock(&GTitleMutex);
    return GModalTitle;
}

bool ShouldReportBlocked(double ThresholdSeconds)
{
    return IsBlocked() && BlockedSeconds() >= ThresholdSeconds;
}

double GameThreadStalledSeconds()
{
    const double Last = GLastAliveSeconds.load(std::memory_order_acquire);
    if (Last <= 0.0)
    {
        // Never stamped: the subsystem ticker has not run once yet. There is no
        // heartbeat to be stale, so this is not a stall.
        return 0.0;
    }
    return FMath::Max(0.0, FPlatformTime::Seconds() - Last);
}

bool ShouldReportGameThreadStalled(double ThresholdSeconds)
{
    // A zero or negative threshold disables the probe. Without this guard it
    // would instead report a permanent stall, because the >= comparison is
    // satisfied by the 0.0 that a never-stamped heartbeat returns.
    if (ThresholdSeconds <= 0.0)
    {
        return false;
    }
    return GameThreadStalledSeconds() >= ThresholdSeconds;
}

void GetInFlightRpc(FString& OutMethod, FString& OutRequestId, double& OutSeconds)
{
    double Start = 0.0;
    {
        FScopeLock Lock(&GInFlightMutex);
        OutMethod = GInFlightMethod;
        OutRequestId = GInFlightRequestId;
        Start = GInFlightStartSeconds;
    }
    OutSeconds = Start > 0.0
        ? FMath::Max(0.0, FPlatformTime::Seconds() - Start)
        : 0.0;
}

void ResetForTests()
{
    GBlocked.store(false, std::memory_order_release);
    GBlockStartSeconds.store(0.0, std::memory_order_release);
    SetTitle(FString());
    GLastAliveSeconds.store(0.0, std::memory_order_release);
    NoteRpcDispatchEnd();
}

void SetLastAliveSecondsForTests(double AbsoluteSeconds)
{
    GLastAliveSeconds.store(AbsoluteSeconds, std::memory_order_release);
}
}
