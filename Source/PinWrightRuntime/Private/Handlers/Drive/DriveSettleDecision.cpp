// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/Drive/DriveSettleDecision.h"

EDriveSettleOutcome StepSettle(
    FDriveSettleState& State,
    bool bChangedSinceBaseline,
    bool bStableSinceLastTick,
    bool bWaitForMet,
    int32 ElapsedMs,
    const FDriveSettleConfig& Config,
    FDriveSettleResult& OutResult)
{
    // Each call is one observation tick.
    ++State.Ticks;

    // Latch change tracking in both modes so OutResult.bChanged is always
    // meaningful once the UI has diverged from the baseline.
    const bool bChangedBefore = State.bHasChanged;
    State.bHasChanged |= bChangedSinceBaseline;
    if (bChangedBefore)
    {
        ++State.TicksSinceChange;
    }

    // Local-only result filler. A lambda (not a free/anonymous-namespace helper)
    // keeps it ODR-safe under Unity builds.
    auto Finish = [&State, ElapsedMs, &OutResult](
        bool bChanged, bool bSettled, bool bConditionMet, EDriveSettleOutcome Outcome)
    {
        OutResult.bChanged = bChanged;
        OutResult.bSettled = bSettled;
        OutResult.bConditionMet = bConditionMet;
        OutResult.ElapsedMs = ElapsedMs;
        OutResult.Ticks = State.Ticks;
        OutResult.Outcome = Outcome;
        return Outcome;
    };

    // Explicit wait_for: the only terminal states are the condition itself and
    // its dedicated timeout. Change/stability tracking is irrelevant here.
    if (Config.WaitFor.IsSet())
    {
        if (bWaitForMet)
        {
            return Finish(State.bHasChanged, /*bSettled=*/false,
                /*bConditionMet=*/true, EDriveSettleOutcome::WaitForMet);
        }
        if (ElapsedMs >= Config.WaitForTimeoutMs)
        {
            return Finish(State.bHasChanged, /*bSettled=*/false,
                /*bConditionMet=*/false, EDriveSettleOutcome::Timeout);
        }
        return Finish(State.bHasChanged, /*bSettled=*/false,
            /*bConditionMet=*/false, EDriveSettleOutcome::Continue);
    }

    // No wait_for: settle on change-then-stable, else report a clean quiet.
    if (State.bHasChanged)
    {
        // Count consecutive stable ticks since the change; any fresh change
        // (a non-stable tick) restarts the count.
        if (bStableSinceLastTick)
        {
            ++State.ConsecutiveStableTicks;
        }
        else
        {
            State.ConsecutiveStableTicks = 0;
        }

        if (State.ConsecutiveStableTicks >= Config.StableTicks)
        {
            return Finish(/*bChanged=*/true, /*bSettled=*/true,
                /*bConditionMet=*/false, EDriveSettleOutcome::SettledChanged);
        }
        // Stabilization wins over the budget on the same tick: only give up
        // once we are sure we did not just settle. The budget also waits until
        // StableTicks ticks have run after the change, so a frame hitch that shows
        // the change only as the budget runs out still gets the chance to see the
        // UI hold still. That wait ends at twice the budget, so a large
        // stable_ticks or a run of slow frames cannot hold the response open.
        const bool bStableWindowRan = State.TicksSinceChange >= Config.StableTicks;
        const bool bPastCeiling = static_cast<int64>(ElapsedMs) >= 2 * static_cast<int64>(Config.SettleBudgetMs);
        if (ElapsedMs >= Config.SettleBudgetMs && (bStableWindowRan || bPastCeiling))
        {
            return Finish(/*bChanged=*/true, /*bSettled=*/false,
                /*bConditionMet=*/false, EDriveSettleOutcome::ChangedUnsettled);
        }
        return Finish(/*bChanged=*/true, /*bSettled=*/false,
            /*bConditionMet=*/false, EDriveSettleOutcome::Continue);
    }

    // Never changed: once the quiet budget elapses, report a clean no-change.
    if (ElapsedMs >= Config.QuietBudgetMs)
    {
        return Finish(/*bChanged=*/false, /*bSettled=*/false,
            /*bConditionMet=*/false, EDriveSettleOutcome::NoChangeWithinBudget);
    }
    return Finish(/*bChanged=*/false, /*bSettled=*/false,
        /*bConditionMet=*/false, EDriveSettleOutcome::Continue);
}
