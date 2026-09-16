// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/Niagara/NiagaraCompileWait.h"

#include "Compat/EngineVersionCompat.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "NiagaraEmitter.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraSystem.h"
#include "Utils/AssetCompilePump.h"
// FVersionedNiagaraEmitter is defined here (NiagaraCommon.h only forward-declares it).
#include "NiagaraTypes.h"
#include "UObject/UObjectIterator.h"

namespace PinWrightNiagara
{
    namespace
    {
        // GPU shader compilation is excluded from the wait predicate on purpose. The corruption
        // this wait exists to prevent is in the VM script's compiled DataInterfaceInfo list,
        // which is populated by the CPU-script compile; blocking on GPU shaders as well would
        // add minutes to every `{compile:true, save:true}` edit for a hazard that is not the one
        // being closed. Callers who need the GPU state can read `niagara.validate`, which
        // reports HasOutstandingCompilationRequests(true).
        constexpr bool GWaitIncludesGpuShaders = false;
        constexpr float GCompileWaitPollIntervalSeconds = 0.01f;

        struct FCompileWaitTarget
        {
            TWeakObjectPtr<UNiagaraSystem> System;
            FString Path;
            bool bMayFlushRequestCompile = false;
        };

        bool UsesEmitter(const UNiagaraSystem& System, const UNiagaraEmitter& Emitter, const FGuid& VersionGuid)
        {
            const FVersionedNiagaraEmitter VersionedEmitter(const_cast<UNiagaraEmitter*>(&Emitter), VersionGuid);
            return System.UsesEmitter(VersionedEmitter);
        }

        bool HasWaitWork(const UNiagaraSystem& System, bool bMayFlushRequestCompile)
        {
            if (bMayFlushRequestCompile)
            {
                return HasPendingCompileWork(System);
            }
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 5, 0)
            // A false request result may coexist with an older active compile. Observe that work,
            // but do not turn a merely pending on-demand request into work this call may not start.
            return System.HasActiveCompilations();
#else
            // HasActiveCompilations arrived in 5.5 and is exactly !ActiveCompilations.IsEmpty().
            // The same distinction is reachable through public API before that:
            // HasOutstandingCompilationRequests(false) is `bNeedsRequestCompile ||
            // ActiveCompilations.Num() > 0` (NiagaraSystem.cpp:1775 on 5.4) and
            // NeedsRequestCompile() reads the first term, so subtracting it leaves the second.
            // The one state the pair cannot separate is a queued request that coexists with an
            // active compile; that reports no work, which errs toward NOT starting a request this
            // call may not start - the property this branch exists to protect.
            return HasPendingCompileWork(System) && !System.NeedsRequestCompile();
#endif
        }

        void AddWaitTarget(
            TArray<FCompileWaitTarget>& Targets,
            UNiagaraSystem& System,
            bool bMayFlushRequestCompile)
        {
            for (FCompileWaitTarget& Target : Targets)
            {
                if (Target.System.Get() == &System)
                {
                    Target.bMayFlushRequestCompile |= bMayFlushRequestCompile;
                    return;
                }
            }

            FCompileWaitTarget& Target = Targets.Emplace_GetRef();
            Target.System = &System;
            Target.Path = System.GetPathName();
            Target.bMayFlushRequestCompile = bMayFlushRequestCompile;
        }

        void RefreshWaitState(
            const TArray<FCompileWaitTarget>& Targets,
            FCompileWaitOutcome& Outcome)
        {
            Outcome.StillCompiling.Reset();
            for (const FCompileWaitTarget& Target : Targets)
            {
                const UNiagaraSystem* System = Target.System.Get();
                if (!System || HasWaitWork(*System, Target.bMayFlushRequestCompile))
                {
                    Outcome.StillCompiling.Add(Target.Path);
                }
            }
            Outcome.StillCompiling.Sort();
            Outcome.bOutstanding = !Outcome.StillCompiling.IsEmpty();
        }

        FCompileWaitOutcome WaitForTargets(
            const TArray<FCompileWaitTarget>& Targets,
            double TimeoutSeconds)
        {
            FCompileWaitOutcome Outcome;
            const double StartSeconds = FPlatformTime::Seconds();
            const double DeadlineSeconds = StartSeconds + FMath::Max(0.0, TimeoutSeconds);
            RefreshWaitState(Targets, Outcome);

            while (Outcome.bOutstanding)
            {
                if (FPlatformTime::Seconds() >= DeadlineSeconds)
                {
                    Outcome.bTimedOut = true;
                    break;
                }

                Outcome.bWaited = true;
                // Niagara registers FNiagaraSystemCompilingManager with this public manager.
                // Its game-thread pass drains queued functions, advances compile tasks, and
                // applies finished results without entering Niagara's unbounded wait API.
                PinWright::AssetCompile::AdvanceOnGameThread();
                for (const FCompileWaitTarget& Target : Targets)
                {
                    if (UNiagaraSystem* System = Target.System.Get())
                    {
                        if (HasWaitWork(*System, Target.bMayFlushRequestCompile))
                        {
                            System->PollForCompilationComplete(Target.bMayFlushRequestCompile);
                        }
                    }
                }

                RefreshWaitState(Targets, Outcome);
                if (!Outcome.bOutstanding)
                {
                    break;
                }

                const double RemainingSeconds = DeadlineSeconds - FPlatformTime::Seconds();
                if (RemainingSeconds <= 0.0)
                {
                    Outcome.bTimedOut = true;
                    break;
                }
                FPlatformProcess::SleepNoStats(
                    static_cast<float>(FMath::Min<double>(GCompileWaitPollIntervalSeconds, RemainingSeconds)));
            }

            Outcome.WaitedSeconds = FPlatformTime::Seconds() - StartSeconds;
            RefreshWaitState(Targets, Outcome);
            Outcome.bTimedOut &= Outcome.bOutstanding;
            return Outcome;
        }
    }

    bool HasPendingCompileWork(const UNiagaraSystem& System)
    {
        return System.HasOutstandingCompilationRequests(GWaitIncludesGpuShaders);
    }

    bool HasGpuComputeSimulation(const UNiagaraSystem& System)
    {
        for (const FNiagaraEmitterHandle& Handle : System.GetEmitterHandles())
        {
            const FVersionedNiagaraEmitterData* EmitterData = Handle.GetEmitterData();
            if (EmitterData
                && EmitterData->SimTarget == ENiagaraSimTarget::GPUComputeSim
                && EmitterData->GetGPUComputeScript())
            {
                return true;
            }
        }
        return false;
    }

    FCompileWaitOutcome WaitForSystemCompile(
        UNiagaraSystem& System,
        bool bMayFlushRequestCompile,
        double TimeoutSeconds)
    {
        TArray<FCompileWaitTarget> Targets;
        AddWaitTarget(Targets, System, bMayFlushRequestCompile);
        return WaitForTargets(Targets, TimeoutSeconds);
    }

    bool IsSystemReadyAfterCompileWait(
        const UNiagaraSystem& System,
        const FCompileWaitOutcome& Outcome,
        bool bExplicitCompileFailure)
    {
        return !bExplicitCompileFailure
            && !Outcome.bTimedOut
            && !Outcome.bOutstanding
            && !HasPendingCompileWork(System)
            && System.IsReadyToRun();
    }

    FCompileWaitOutcome WaitForRequestedSystemCompiles(
        const TArray<TWeakObjectPtr<UNiagaraSystem>>& RequestedSystems,
        double TimeoutSeconds)
    {
        TArray<FCompileWaitTarget> Targets;
        for (const TWeakObjectPtr<UNiagaraSystem>& RequestedSystem : RequestedSystems)
        {
            if (UNiagaraSystem* System = RequestedSystem.Get())
            {
                AddWaitTarget(Targets, *System, /*bMayFlushRequestCompile=*/true);
            }
        }
        return WaitForTargets(Targets, TimeoutSeconds);
    }

    FCompileWaitOutcome WaitForEmitterCompile(
        const UNiagaraEmitter& Emitter,
        const FGuid& VersionGuid,
        const TArray<TWeakObjectPtr<UNiagaraSystem>>& RequestedSystems,
        double TimeoutSeconds)
    {
        TSet<UNiagaraSystem*> NewlyRequestedSystems;
        for (const TWeakObjectPtr<UNiagaraSystem>& RequestedSystem : RequestedSystems)
        {
            if (UNiagaraSystem* System = RequestedSystem.Get())
            {
                NewlyRequestedSystems.Add(System);
            }
        }

        TArray<FCompileWaitTarget> Targets;
        for (TObjectIterator<UNiagaraSystem> It; It; ++It)
        {
            UNiagaraSystem* System = *It;
            if (System && UsesEmitter(*System, Emitter, VersionGuid))
            {
                AddWaitTarget(Targets, *System, NewlyRequestedSystems.Contains(System));
            }
        }
        // Preserve an actual launched request even if its system stopped referencing the emitter
        // between the request sweep and this wait sweep.
        for (UNiagaraSystem* RequestedSystem : NewlyRequestedSystems)
        {
            AddWaitTarget(Targets, *RequestedSystem, /*bMayFlushRequestCompile=*/true);
        }
        return WaitForTargets(Targets, TimeoutSeconds);
    }

    FEmitterCompileObservation ObserveEmitterCompiles(
        const UNiagaraEmitter& Emitter,
        const FGuid& VersionGuid)
    {
        FEmitterCompileObservation Observation;
        for (TObjectIterator<UNiagaraSystem> It; It; ++It)
        {
            const UNiagaraSystem* System = *It;
            if (System && UsesEmitter(*System, Emitter, VersionGuid))
            {
                ++Observation.AffectedSystemCount;
                Observation.bCpuScriptCompilationPending |=
                    System->HasOutstandingCompilationRequests(/*bIncludingGPUShaders=*/false);
                Observation.bAnyCompilationPending |=
                    System->HasOutstandingCompilationRequests(/*bIncludingGPUShaders=*/true);
                Observation.bIncludesGpuComputeSimulation |= HasGpuComputeSimulation(*System);
            }
        }
        return Observation;
    }

    bool HasOutstandingEmitterCompile(const UNiagaraEmitter& Emitter, const FGuid& VersionGuid)
    {
        return ObserveEmitterCompiles(Emitter, VersionGuid).bCpuScriptCompilationPending;
    }

    bool MayPersistAfterCompileWait(const FCompileWaitOutcome& Outcome)
    {
        return !Outcome.bTimedOut && !Outcome.bOutstanding;
    }

    bool DidCompileLand(bool bRequested, const FCompileWaitOutcome& Outcome)
    {
        return bRequested && !Outcome.bTimedOut && !Outcome.bOutstanding;
    }

    const TCHAR* DescribeCompileOutcome(bool bRequested, const FCompileWaitOutcome& Outcome)
    {
        if (Outcome.bTimedOut)
        {
            return TEXT("timedOut");
        }
        if (Outcome.bOutstanding)
        {
            return TEXT("outstanding");
        }
        if (!bRequested)
        {
            return TEXT("notRequested");
        }
        return TEXT("completed");
    }
}
