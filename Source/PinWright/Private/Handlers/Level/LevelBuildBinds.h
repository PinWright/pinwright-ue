// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "Dom/JsonObject.h"
#include "Handlers/HandlerContext.h"
#include "Editor.h"
#include "EditorBuildUtils.h"
#include "Engine/World.h"
#include "NavigationSystem.h"

// Per-tick decision for a build-completion watchdog (below). Pure and
// side-effect free so it can be unit-tested directly without driving a real
// editor build. Domain-neutral: it drives both the lighting-build watchdog
// (whose FEditorDelegates::OnLightingBuild* delegates are one-shot/in-memory
// and can be missed on editor stall, GC/serialization deferral, or
// kill+relaunch) and the navigation-build watchdog (whose dynamic delegate has
// no AddLambda, so it can only be polled), which would otherwise leave a job
// "running" forever.
enum class EBuildWatchdogAction : uint8
{
    Continue,         // keep polling
    Stop,             // delegate already reconciled / gave up — remove ticker, do not resolve
    ResolveAndStop,   // build ran then stopped with no delegate firing — reconcile job, remove ticker
};

// The pre-start wait before a watchdog gives up: 12 ticks at 0.5 s/tick (~6 s).
// Shared by every build watchdog so the give-up timeout has one definition.
inline constexpr int32 GBuildWatchdogMaxWarmupTicks = 12;

// bDelegateFired: a completion delegate has already resolved the job.
// bBuildRunning:  the engine's "build running" flag this tick (lighting:
//   FEditorBuildUtils::IsBuildCurrentlyRunning(); nav: IsNavigationBuildInProgress()).
// bHasSeenBuildStart (in/out): set true once the build has been observed running.
// WarmupTicks (in/out): pre-start wait counter; capped at MaxWarmupTicks.
inline EBuildWatchdogAction StepBuildWatchdog(
    bool bDelegateFired,
    bool bBuildRunning,
    bool& bHasSeenBuildStart,
    int32& WarmupTicks,
    int32 MaxWarmupTicks = GBuildWatchdogMaxWarmupTicks)
{
    if (bDelegateFired)
    {
        return EBuildWatchdogAction::Stop;
    }
    if (bBuildRunning)
    {
        bHasSeenBuildStart = true;
        return EBuildWatchdogAction::Continue;
    }
    if (bHasSeenBuildStart)
    {
        // Build ran and is no longer running, yet no completion delegate fired —
        // reconcile to terminal so the job never hangs.
        return EBuildWatchdogAction::ResolveAndStop;
    }
    // Build hasn't started yet; cap the pre-start wait so we never poll forever.
    ++WarmupTicks;
    if (WarmupTicks >= MaxWarmupTicks)
    {
        return EBuildWatchdogAction::Stop;
    }
    return EBuildWatchdogAction::Continue;
}

inline TFunction<void(FJobOnComplete)> BindLightingBuildCompletion()
{
    return [](FJobOnComplete OnComplete)
    {
        TSharedRef<TArray<FDelegateHandle>, ESPMode::ThreadSafe> Handles
            = MakeShared<TArray<FDelegateHandle>, ESPMode::ThreadSafe>();
        TSharedRef<bool, ESPMode::ThreadSafe> bFired
            = MakeShared<bool, ESPMode::ThreadSafe>(false);

        auto Resolve = [OnComplete, Handles, bFired]
            (bool bSuccess, FString Outcome) mutable
        {
            if (*bFired) return;
            *bFired = true;
            for (FDelegateHandle& H : *Handles)
            {
                FEditorDelegates::OnLightingBuildSucceeded.Remove(H);
                FEditorDelegates::OnLightingBuildFailed.Remove(H);
                FEditorDelegates::OnLightingBuildKept.Remove(H);
            }
            Handles->Reset();
            auto R = MakeShared<FJsonObject>();
            R->SetStringField(TEXT("outcome"), Outcome);
            OnComplete(bSuccess, R, bSuccess ? FString() : Outcome);
        };

        Handles->Add(FEditorDelegates::OnLightingBuildSucceeded.AddLambda(
            [Resolve]() mutable { Resolve(true, TEXT("succeeded")); }));
        Handles->Add(FEditorDelegates::OnLightingBuildFailed.AddLambda(
            [Resolve]() mutable { Resolve(false, TEXT("failed")); }));
        Handles->Add(FEditorDelegates::OnLightingBuildKept.AddLambda(
            [Resolve]() mutable { Resolve(true, TEXT("kept")); }));

        // Watchdog fallback: the three FEditorDelegates above are one-shot,
        // in-memory, and broadcast from a single editor-frame tick of the
        // static-lighting state machine. If that broadcast is ever missed
        // (editor stall/recovery, GC/serialization deferral, or a completion
        // route that does not broadcast these delegates) the job would stay
        // "running" forever with no terminal state. Poll the engine's real
        // build-running flag and reconcile through the SAME first-fires-wins
        // Resolve guard, so on the happy path the delegate still wins and this
        // ticker no-ops. bHasSeenBuildStart/WarmupTicks are game-thread-only
        // scratch owned solely by this mutable ticker lambda — captured by value,
        // they persist across ticks (the FTSTicker holds one lambda copy).
        FTSTicker::GetCoreTicker().AddTicker(
            FTickerDelegate::CreateLambda(
                [Resolve, bFired, bHasSeenBuildStart = false, WarmupTicks = 0](float) mutable -> bool
        {
            const EBuildWatchdogAction Action = StepBuildWatchdog(
                *bFired,
                FEditorBuildUtils::IsBuildCurrentlyRunning(),
                bHasSeenBuildStart,
                WarmupTicks);
            switch (Action)
            {
            case EBuildWatchdogAction::ResolveAndStop:
                // Distinct outcome so testers can tell the fallback fired.
                Resolve(true, TEXT("completed_via_poll"));
                return false; // remove ticker
            case EBuildWatchdogAction::Stop:
                return false; // remove ticker (delegate path remains bound)
            case EBuildWatchdogAction::Continue:
            default:
                return true; // keep polling (~0.5 s/tick)
            }
        }), 0.5f);
    };
}

inline TFunction<void(FJobOnComplete)> BindNavigationBuildCompletion(UWorld* World)
{
    return [World](FJobOnComplete OnComplete)
    {
        UNavigationSystemV1* NavSys = UNavigationSystemV1::GetNavigationSystem(World);
        if (!NavSys)
        {
            OnComplete(false, nullptr, TEXT("NO_NAV_SYSTEM"));
            return;
        }
        // Poll until the navigation build finishes. AddLambda is not available on
        // DECLARE_DYNAMIC_MULTICAST_DELEGATE, so we drive the SAME poll watchdog as
        // the lighting bind (StepBuildWatchdog) off a half-second ticker — there is
        // no completion delegate here, so bDelegateFired is always false and a Stop
        // can only mean warmup exhaustion (the build never started). bHasSeenBuildStart
        // guards against premature completion before the build begins; together with
        // WarmupTicks it is game-thread-only scratch owned by this mutable lambda
        // (captured by value, persists across ticks).
        FTSTicker::GetCoreTicker().AddTicker(
            FTickerDelegate::CreateLambda(
                [WeakNav = TWeakObjectPtr<UNavigationSystemV1>(NavSys),
                 OnComplete, bHasSeenBuildStart = false, WarmupTicks = 0](float) mutable -> bool
        {
            UNavigationSystemV1* Live = WeakNav.Get();
            if (!Live)
            {
                OnComplete(false, nullptr, TEXT("NAV_SYSTEM_GONE"));
                return false; // remove ticker
            }
            const EBuildWatchdogAction Action = StepBuildWatchdog(
                /*bDelegateFired=*/false,
                Live->IsNavigationBuildInProgress(),
                bHasSeenBuildStart,
                WarmupTicks);
            switch (Action)
            {
            case EBuildWatchdogAction::ResolveAndStop:
            {
                // Build ran then stopped.
                auto R = MakeShared<FJsonObject>();
                R->SetBoolField(TEXT("nav_built"), true);
                OnComplete(true, R, FString());
                return false; // build finished, remove ticker
            }
            case EBuildWatchdogAction::Stop:
            {
                // No build was ever seen within the warmup cap (~6 s) — treat as a
                // no-op (no nav meshes). Nav has no delegate, so Stop is reachable
                // only via warmup exhaustion.
                auto R = MakeShared<FJsonObject>();
                R->SetBoolField(TEXT("nav_built"), true);
                R->SetBoolField(TEXT("nothing_to_build"), true);
                OnComplete(true, R, FString());
                return false;
            }
            case EBuildWatchdogAction::Continue:
            default:
                return true; // keep polling
            }
        }), 0.5f);
    };
}
