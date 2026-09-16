// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"

// Bounded retry schedule for a listener port that was held by someone else at startup.
//
// THE DEFECT THIS EXISTS FOR. FSocketHttpServer::Start bound the port exactly once. When an
// editor died in the render thread and a replacement booted before the old process released
// the socket, the replacement logged one `Port %u could not be bound` Error and then served
// MCP never again - a live editor process that reads to every caller as "editor down" while
// tasklist shows it running.
//
// Everything here is pure: it takes the current time as an argument and owns no clock, no
// socket and no editor. That is what makes the schedule testable without any of them - the
// retry bug is a *timing policy* bug, and a test that could only observe it through a real
// bind would not be able to fail.
//
// Public rather than Private only because UPinWrightSubsystem holds an FState by value and
// that header is Public; nothing outside the module is expected to use this.
namespace PinWrightBindRetry
{

// What the owner should do on this tick.
enum class EAction : uint8
{
    // Nothing due yet, or retrying is over (bound, or given up and the re-log is not due).
    None,
    // Attempt the bind now, then report back through NoteAttemptFailed / NoteSucceeded.
    Attempt,
    // The budget is spent. Re-emit the terminal diagnostic; no further binds will be tried.
    Relog
};

// The numbers, with their justification. Defaults are the shipped policy.
struct FPolicy
{
    // Delay before the FIRST retry. The measured case is a crashed process still holding the
    // socket while it tears down, so the first retry wants to be soon enough to catch a fast
    // exit without adding a perceptible stall to a normal boot.
    double FirstDelaySeconds = 2.0;

    // Each subsequent delay doubles, clamped here. Contention that survives half a minute is
    // not a process exiting, so retrying faster than this only fills the log.
    double MaxDelaySeconds = 30.0;

    // Total retry budget, measured from the first failed bind. Covers a slow editor teardown
    // including a crash-reporter dialog somebody has to dismiss. It is deliberately BOUNDED:
    // when the port belongs to another project's editor, or sits in a Windows excluded /
    // Hyper-V reserved range, no amount of waiting fixes it, and retrying forever would hide
    // a configuration problem behind an infinite "still trying" that never names the fix.
    double TotalBudgetSeconds = 600.0;

    // After the budget is spent, re-emit the terminal error on this cadence rather than
    // leaving one line at startup. An agent that attaches to the log an hour later must still
    // be able to see why this editor is not serving. Rare enough not to be noise.
    double ExhaustedRelogSeconds = 300.0;

    // Delay before retry number Attempt (1 = the first retry after the startup bind failed).
    // Doubling from FirstDelaySeconds, clamped to MaxDelaySeconds.
    double DelayForAttempt(int32 Attempt) const
    {
        if (Attempt <= 1)
        {
            return FMath::Min(FirstDelaySeconds, MaxDelaySeconds);
        }
        double Delay = FirstDelaySeconds;
        for (int32 Step = 1; Step < Attempt && Delay < MaxDelaySeconds; ++Step)
        {
            Delay *= 2.0;
        }
        return FMath::Min(Delay, MaxDelaySeconds);
    }
};

// Retry bookkeeping driven by a caller-supplied clock.
//
// Lifecycle: Begin on the startup bind failure, then Advance every tick and answer its
// EAction::Attempt with exactly one of NoteAttemptFailed / NoteSucceeded.
class FState
{
public:
    // The startup bind has failed. Now is the caller's clock reading at that moment; it
    // counts as failed attempt #1, and the budget is measured from it.
    void Begin(double Now, const FPolicy& InPolicy)
    {
        Policy = InPolicy;
        bRetrying = true;
        bExhausted = false;
        FailedAttempts = 1;
        StartSeconds = Now;
        NextAttemptSeconds = Now + Policy.DelayForAttempt(1);
        NextRelogSeconds = 0.0;
    }

    EAction Advance(double Now)
    {
        if (!bRetrying)
        {
            return EAction::None;
        }
        if (bExhausted)
        {
            if (Now >= NextRelogSeconds)
            {
                NextRelogSeconds = Now + Policy.ExhaustedRelogSeconds;
                return EAction::Relog;
            }
            return EAction::None;
        }
        return (Now >= NextAttemptSeconds) ? EAction::Attempt : EAction::None;
    }

    // The attempt Advance authorised did not bind. Schedules the next one, or spends the
    // budget. The budget is checked against the time the NEXT attempt would land rather than
    // against now, so the schedule never contains an attempt beyond it.
    void NoteAttemptFailed(double Now)
    {
        if (!bRetrying || bExhausted)
        {
            return;
        }
        ++FailedAttempts;
        const double Candidate = Now + Policy.DelayForAttempt(FailedAttempts);
        if ((Candidate - StartSeconds) > Policy.TotalBudgetSeconds)
        {
            bExhausted = true;
            // Due immediately: the terminal diagnostic is emitted on the tick that gives up,
            // then repeats on the ExhaustedRelogSeconds cadence.
            NextRelogSeconds = Now;
            return;
        }
        NextAttemptSeconds = Candidate;
    }

    // Bound. Retrying stops for good; a later Stop/Start is a fresh Begin.
    void NoteSucceeded() { Reset(); }

    // Abandon the schedule without a verdict (transport torn down).
    void Reset()
    {
        bRetrying = false;
        bExhausted = false;
    }

    // True while the schedule is live, INCLUDING after the budget is spent - an exhausted
    // state still has a re-log to emit and is still the reason this editor is not serving.
    bool IsRetrying() const { return bRetrying; }

    // True once the budget is spent: no further bind will be attempted.
    bool IsExhausted() const { return bExhausted; }

    // Failed bind attempts so far, counting the startup one as the first.
    int32 GetFailedAttempts() const { return FailedAttempts; }

    double GetElapsedSeconds(double Now) const { return bRetrying ? (Now - StartSeconds) : 0.0; }

    // Clock reading the next bind is due at. Meaningless once exhausted.
    double GetNextAttemptSeconds() const { return NextAttemptSeconds; }

    const FPolicy& GetPolicy() const { return Policy; }

private:
    FPolicy Policy;
    bool bRetrying = false;
    bool bExhausted = false;
    int32 FailedAttempts = 0;
    double StartSeconds = 0.0;
    double NextAttemptSeconds = 0.0;
    double NextRelogSeconds = 0.0;
};

} // namespace PinWrightBindRetry
