// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/Drive/DriveSettleDriver.h"

#include "Containers/Ticker.h"
#include "HAL/PlatformTime.h"

TSharedRef<FDriveSettleDriver> FDriveSettleDriver::CreateForStep(
    const FDriveSettleConfig& Config,
    FGetElements GetElements,
    FIsWaitForMet IsWaitForMet,
    FOnComplete OnComplete,
    double StartSeconds)
{
    TSharedRef<FDriveSettleDriver> Driver = MakeShared<FDriveSettleDriver>();
    Driver->Config = Config;
    Driver->GetElements = MoveTemp(GetElements);
    Driver->IsWaitForMet = MoveTemp(IsWaitForMet);
    Driver->OnComplete = MoveTemp(OnComplete);
    Driver->InitBaseline(StartSeconds);
    return Driver;
}

TSharedRef<FDriveSettleDriver> FDriveSettleDriver::Start(
    const FDriveSettleConfig& Config,
    FGetElements GetElements,
    FIsWaitForMet IsWaitForMet,
    FOnComplete OnComplete)
{
    TSharedRef<FDriveSettleDriver> Driver = CreateForStep(
        Config, MoveTemp(GetElements), MoveTemp(IsWaitForMet), MoveTemp(OnComplete),
        FPlatformTime::Seconds());

    // Per-frame (0.0s) ticker. The lambda captures the driver ref by value, which
    // is the sole owning reference once the caller drops the returned ref: Tick()
    // returning false removes the ticker, releases the lambda, and frees the
    // driver. Mirrors the by-value scratch + return-false removal in
    // Level/LevelBuildBinds.h.
    FTSTicker::GetCoreTicker().AddTicker(
        FTickerDelegate::CreateLambda([Driver](float) -> bool
        {
            return Driver->Tick(FPlatformTime::Seconds());
        }), 0.0f);

    return Driver;
}

void FDriveSettleDriver::InitBaseline(double InStartSeconds)
{
    StartSeconds = InStartSeconds;
    const TArray<FDriveElement> Initial = GetElements ? GetElements() : TArray<FDriveElement>();
    Baseline = FDriveChangeDetector::Compute(Initial);
    // First tick's "stable since last tick" compares against the baseline.
    LastTickFingerprint = Baseline;
}

bool FDriveSettleDriver::Tick(double NowSeconds)
{
    // Never re-enter after a terminal outcome (e.g. a stray ticker callback).
    if (bComplete)
    {
        return false;
    }

    const TArray<FDriveElement> Current = GetElements ? GetElements() : TArray<FDriveElement>();
    const FDriveFingerprint Fingerprint = FDriveChangeDetector::Compute(Current);

    const bool bChangedSinceBaseline = (Fingerprint != Baseline);
    const bool bStableSinceLastTick = (Fingerprint == LastTickFingerprint);
    const bool bWaitForMet = IsWaitForMet ? IsWaitForMet() : false;

    // Real elapsed time since start, in whole ms. Round (not truncate) so binary
    // floating-point representation of round seconds does not shave a millisecond
    // off a budget comparison.
    const double ElapsedMsD = FMath::RoundToDouble((NowSeconds - StartSeconds) * 1000.0);
    const int32 ElapsedMs = static_cast<int32>(FMath::Max(0.0, ElapsedMsD));

    FDriveSettleResult Result;
    const EDriveSettleOutcome Outcome = StepSettle(
        State,
        bChangedSinceBaseline,
        bStableSinceLastTick,
        bWaitForMet,
        ElapsedMs,
        Config,
        Result);

    LastTickFingerprint = Fingerprint;

    if (Outcome == EDriveSettleOutcome::Continue)
    {
        return true; // keep ticking
    }

    // Terminal: fire OnComplete exactly once with the result and the latest sample.
    bComplete = true;
    if (OnComplete)
    {
        OnComplete(Result, Current);
    }
    return false; // remove the ticker
}
