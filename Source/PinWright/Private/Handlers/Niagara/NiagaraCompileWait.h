// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"

class UNiagaraEmitter;
class UNiagaraSystem;

namespace PinWrightNiagara
{
    inline constexpr double DefaultCompileWaitTimeoutSeconds = 60.0;
    inline constexpr double MinCompileWaitTimeoutSeconds = 0.01;
    inline constexpr double MaxCompileWaitTimeoutSeconds = 60.0;

    // What a synchronous wait actually observed. Every field is measured, never assumed: the whole
    // point of this struct is that `niagara.compile` used to publish `compiled: true` beside
    // `status: "requested"` roughly 10 ms after issuing a compile that landed seconds later.
    struct FCompileWaitOutcome
    {
        // The engine-owned completion wait was entered with work in flight.
        bool bWaited = false;
        // HasOutstandingCompilationRequests(), measured after the wait returned.
        bool bOutstanding = false;
        // The caller's wall-clock budget expired while one or more systems remained outstanding.
        bool bTimedOut = false;
        // Wall-clock seconds spent inside the wait.
        double WaitedSeconds = 0.0;
        // Exact system object paths still outstanding when the wait returned.
        TArray<FString> StillCompiling;
    };

    // The one compile-readiness predicate. Whatever refuses a caller and whatever the same
    // response names as its compile state must both read this: HasOutstandingCompilationRequests
    // answers differently with and without GPU shaders in scope, and a refusal measured on one
    // answer while publishing the other sends the caller after work that is not blocking it.
    // GPU shaders stay out of scope because the engine's own IsReadyToRunInternal() excludes them.
    bool HasPendingCompileWork(const UNiagaraSystem& System);

    // Does this system carry GPU simulation work at all? The compile probes report whether their
    // outstanding flag had GPU shader compilation in scope; on a CPU-only system that scope is
    // empty, and publishing a bare `true` there reads as pending GPU work the asset cannot have.
    bool HasGpuComputeSimulation(const UNiagaraSystem& System);

    // Advance Niagara through the registered asset-compilation manager until this system becomes
    // quiet or the caller's budget expires. bMayFlushRequestCompile decides whether a merely
    // queued request counts as work: draining one issues it (PollForCompilationComplete calls
    // RequestCompile), so only a caller entitled to start that compile may pass true - one that
    // just called RequestCompile itself, or one about to instance the system, which is the demand
    // on-demand compilation defers to. Observing an older active compile never starts a new one.
    FCompileWaitOutcome WaitForSystemCompile(
        UNiagaraSystem& System,
        bool bMayFlushRequestCompile,
        double TimeoutSeconds = DefaultCompileWaitTimeoutSeconds);

    // Single post-wait authority for callers that need to run the system. Unknown per-script
    // status is not itself a failure when Niagara says its cached executable state is runnable;
    // an explicit compiler failure remains a refusal even if older cached bytecode can still run.
    bool IsSystemReadyAfterCompileWait(
        const UNiagaraSystem& System,
        const FCompileWaitOutcome& Outcome,
        bool bExplicitCompileFailure);

    // Wait for exactly the systems whose RequestCompile calls returned true. This is the emitter
    // compile contract: RequestCompileForEmitter returns void and discards both the affected set
    // and each request result, so callers must retain those systems themselves.
    FCompileWaitOutcome WaitForRequestedSystemCompiles(
        const TArray<TWeakObjectPtr<UNiagaraSystem>>& RequestedSystems,
        double TimeoutSeconds = DefaultCompileWaitTimeoutSeconds);

    // Wait for every loaded system that uses the emitter, including a system with an older active
    // compile whose current RequestCompile returned false. RequestedSystems remains a separate
    // truth: only those systems may flush a pending compile request while being polled.
    FCompileWaitOutcome WaitForEmitterCompile(
        const UNiagaraEmitter& Emitter,
        const FGuid& VersionGuid,
        const TArray<TWeakObjectPtr<UNiagaraSystem>>& RequestedSystems,
        double TimeoutSeconds = DefaultCompileWaitTimeoutSeconds);

    struct FEmitterCompileObservation
    {
        int32 AffectedSystemCount = 0;
        bool bCpuScriptCompilationPending = false;
        bool bAnyCompilationPending = false;
        // Any affected system that actually has GPU simulation work, so the broader flag's scope
        // can be reported as measured rather than asserted.
        bool bIncludesGpuComputeSimulation = false;
    };

    // Cheap, non-blocking queue observation across loaded systems using an emitter. The broader
    // flag includes GPU shaders; the CPU flag matches niagara.compile's wait predicate.
    FEmitterCompileObservation ObserveEmitterCompiles(
        const UNiagaraEmitter& Emitter,
        const FGuid& VersionGuid);

    // CPU-script-only compatibility helper used by niagara.compile's wait:false response.
    bool HasOutstandingEmitterCompile(const UNiagaraEmitter& Emitter, const FGuid& VersionGuid);

    // May an asset be written to disk after this wait?
    //
    // No, while a compile is still in flight. UNiagaraScript's presave then finds the script's
    // compiled DataInterfaceInfo list empty while the emitter resolves N of them, logs
    // "Data interface count mismatch during script presave. Invaliding compile results" and
    // writes the invalidated result into the .uasset. Anything that re-ticks that system
    // afterwards executes the VM against a mismatched data-interface table and asserts
    // (`DataSetIdx < ExecCtx->DataSets.Num()`, VectorVMRuntime.cpp) on a task-graph worker -
    // an appError, so the editor process dies for everyone.
    bool MayPersistAfterCompileWait(const FCompileWaitOutcome& Outcome);

    // Did the compile the caller asked for actually land? True only when a compile was
    // requested AND the wait saw it through. Never true for a compile that was merely queued.
    bool DidCompileLand(bool bRequested, const FCompileWaitOutcome& Outcome);

    // Stable wire spelling of the four distinguishable outcomes, so `status` is the single
    // truth a caller can branch on:
    //   "timedOut"     - the bounded wait expired with a compile still in flight
    //   "outstanding"  - a compile is still in flight and was not waited on
    //   "notRequested" - no compile was issued and no compile is in flight (nothing needed
    //                    compiling; `force:false` on a current system takes this path)
    //   "completed"    - a compile was issued and finished
    const TCHAR* DescribeCompileOutcome(bool bRequested, const FCompileWaitOutcome& Outcome);
}
