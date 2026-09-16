// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Handlers/Drive/DriveTypes.h"

// Pure per-tick decision for the drive "settle/wait" loop -- the heart of the
// "no blind sleeps" guarantee. Mirrors the StepBuildWatchdog pattern in
// Level/LevelBuildBinds.h: a side-effect-free function that returns a
// Continue/terminal decision and carries tiny scratch state across ticks. It
// touches no Slate/CEF/UObject and reads no engine clock -- the caller observes
// the live UI each tick and feeds in three booleans plus elapsed milliseconds,
// so the whole decision is unit-testable without driving a real editor.

// Scratch state for a single settle loop, carried across its ticks. The caller
// owns one per in-flight settle (default-constructed at loop start) and passes
// it back every tick. No globals.
struct FDriveSettleState
{
    // Latches true once any divergence from the baseline has been observed.
    bool bHasChanged = false;
    // Consecutive ticks the UI has been stable since the last observed change.
    int32 ConsecutiveStableTicks = 0;
    // Number of StepSettle calls (ticks) so far; surfaced as OutResult.Ticks.
    int32 Ticks = 0;
};

// Advances one settle tick and returns the (possibly terminal) outcome.
//
// State                 in/out scratch (see FDriveSettleState).
// bChangedSinceBaseline did the observed fingerprint differ from the baseline
//                       this tick (i.e. has the UI moved at all yet)?
// bStableSinceLastTick  was the fingerprint identical to the previous tick's?
// bWaitForMet           is Config.WaitFor currently satisfied? Evaluated by a
//                       separate unit and passed in opaquely; ignored when there
//                       is no WaitFor.
// ElapsedMs             milliseconds elapsed since the settle started.
// Config                thresholds/budgets (every threshold comes from here).
// OutResult             always fully populated: bChanged, bSettled,
//                       bConditionMet, ElapsedMs, Ticks, Outcome.
//
// Returns EDriveSettleOutcome::Continue while in progress, or a terminal outcome.
EDriveSettleOutcome StepSettle(
    FDriveSettleState& State,
    bool bChangedSinceBaseline,
    bool bStableSinceLastTick,
    bool bWaitForMet,
    int32 ElapsedMs,
    const FDriveSettleConfig& Config,
    FDriveSettleResult& OutResult);
