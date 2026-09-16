// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Handlers/Drive/DriveTypes.h"
#include "Handlers/Drive/DriveFingerprint.h"
#include "Handlers/Drive/DriveSettleDecision.h"

// Async settle driver for the drive capability: the real-engine ticker half of
// the "no blind sleeps" loop. It owns the per-settle scratch state and wires the
// pure StepSettle decision (DriveSettleDecision.h) to an FTSTicker, sampling the
// live UI fingerprint (DriveFingerprint.h) once per frame until a terminal
// outcome is reached. Mirrors the AddTicker poll-loop in Level/LevelBuildBinds.h:
// by-value scratch carried across ticks, return false to remove the ticker.
//
// The driver is decoupled from the resolver / condition / journal units via
// callbacks: GetElements samples whichever surface the handler targets,
// IsWaitForMet folds the condition engine + journal into one bool, and
// OnComplete delivers the result. The driver itself knows nothing about surfaces
// or conditions.
//
// Testability seam: the per-tick decision lives in Tick(NowSeconds), separate
// from the FTSTicker registration in Start(). A unit test drives Tick() with a
// scripted GetElements sequence and injected times via CreateForStep(), with no
// real ticker and no engine clock. The live ticker callback simply forwards
// FPlatformTime::Seconds() into Tick().
class FDriveSettleDriver
{
public:
    // Samples the current UI surface; called once at start (baseline) and once
    // per tick. The handler binds this to the live element builder.
    using FGetElements = TFunction<TArray<FDriveElement>()>;

    // Evaluates whether Config.WaitFor is currently satisfied. Optional (unbound
    // when there is no wait_for); composed by the handler from the condition
    // engine + journal. Only meaningful when Config.WaitFor is set.
    using FIsWaitForMet = TFunction<bool()>;

    // Delivers the terminal result plus the latest sampled elements. Fires
    // exactly once per settle.
    using FOnComplete = TFunction<void(const FDriveSettleResult&, const TArray<FDriveElement>&)>;

    // Live entry: captures the baseline fingerprint from GetElements(), stamps
    // the start time via FPlatformTime::Seconds(), and registers a per-frame
    // (0.0s interval) FTSTicker that forwards each frame into Tick(). The ticker
    // lambda holds the only required reference to the driver, so the driver lives
    // exactly as long as the settle: when Tick() reaches a terminal outcome it
    // returns false, the ticker is removed, and the driver is destroyed. The
    // returned ref lets the caller hold a handle if it wants one.
    static TSharedRef<FDriveSettleDriver> Start(
        const FDriveSettleConfig& Config,
        FGetElements GetElements,
        FIsWaitForMet IsWaitForMet,
        FOnComplete OnComplete);

    // Test/seam entry: builds the driver and captures the baseline at the given
    // StartSeconds WITHOUT registering any ticker, so a test can drive Tick() on
    // a scripted clock. The live Start() path is implemented in terms of this.
    static TSharedRef<FDriveSettleDriver> CreateForStep(
        const FDriveSettleConfig& Config,
        FGetElements GetElements,
        FIsWaitForMet IsWaitForMet,
        FOnComplete OnComplete,
        double StartSeconds);

    // One settle observation at wall-clock NowSeconds. Samples the current UI via
    // GetElements, computes the change/stability booleans against the baseline and
    // the previous tick, derives ElapsedMs from (NowSeconds - StartSeconds), and
    // runs the pure StepSettle decision. On a terminal outcome it invokes
    // OnComplete exactly once with the result and the latest sample, then never
    // fires again. Returns true to keep ticking, false once terminal (the
    // FTSTicker callback returns this verbatim to remove itself).
    bool Tick(double NowSeconds);

    // True once a terminal outcome has fired OnComplete.
    bool IsComplete() const { return bComplete; }

private:
    // Captures the baseline fingerprint and start time. Samples GetElements once.
    void InitBaseline(double InStartSeconds);

    FDriveSettleConfig Config;
    FGetElements GetElements;
    FIsWaitForMet IsWaitForMet;
    FOnComplete OnComplete;

    // Baseline taken at start; current ticks compare against it for "has the UI
    // moved at all yet". LastTickFingerprint seeds from the baseline so the first
    // tick's "stable since last tick" means "unchanged from baseline".
    FDriveFingerprint Baseline;
    FDriveFingerprint LastTickFingerprint;

    // Pure scratch carried across StepSettle calls.
    FDriveSettleState State;

    // Wall-clock start (FPlatformTime::Seconds units) for the ElapsedMs derivation.
    double StartSeconds = 0.0;

    // Latched once OnComplete has fired; guards against a double-fire if Tick is
    // called again after a terminal outcome.
    bool bComplete = false;
};
