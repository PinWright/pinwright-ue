// Copyright (c) 2026 Alexander Penkin. MIT License.

// NiagaraCompileHandler.cpp
// Handlers: niagara.compile, niagara.compile_status

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/Niagara/NiagaraCompileVerdict.h"
#include "Handlers/Niagara/NiagaraCompileWait.h"
#include "Handlers/Niagara/NiagaraDumpBuilder.h"
#include "Handlers/Niagara/NiagaraEditTypes.h"
#include "Handlers/Niagara/NiagaraRapidIteration.h"
#include "ScopedTransaction.h"
#include "Utils/JsonUtils.h"

#include "HAL/PlatformTime.h"
#include "NiagaraEmitter.h"
#include "NiagaraSystem.h"

// ---- niagara.compile ----
REGISTER_RPC_HANDLER("niagara.compile", "niagara",
    "Compile a Niagara system or emitter asset. wait:true pumps compile work up to timeoutSeconds; wait:false returns after the request so the caller can poll niagara.compile_status.",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath", "path", "Asset path of the Niagara system or emitter to compile"),
        RPC_PARAM_DEF("force",     "boolean",
            "Recompile even when the engine considers the asset current. With force:false a system "
            "the engine already considers up to date issues no compile at all, and the response "
            "reports status \"notRequested\" with compiled:false.", "true"),
        RPC_PARAM_DEF("wait",      "boolean",
            "With true, synchronously pump until Niagara applies the CPU-script result or the time "
            "budget expires. With false, return after requesting and poll niagara.compile_status.", "true"),
        RPC_PARAM_DEF("timeoutSeconds", "number",
            "Maximum wait:true budget in seconds (0.01-60). On expiry, completed is false, timedOut "
            "is true, and stillCompiling lists affected systems to poll with niagara.compile_status.", "60")
    ))
{
    FString AssetPath;
    if (!Ctx.RequireString(TEXT("assetPath"), AssetPath)) return true;

    const bool bWait = Ctx.GetBool(TEXT("wait"), true);
    const bool bForce = Ctx.GetBool(TEXT("force"), true);
    double TimeoutSeconds = PinWrightNiagara::DefaultCompileWaitTimeoutSeconds;
    const TSharedPtr<FJsonObject>& Payload = Ctx.GetRawPayload();
    if (Payload.IsValid() && Payload->HasField(TEXT("timeoutSeconds")))
    {
        if (!Ctx.RequireNumber(TEXT("timeoutSeconds"), TimeoutSeconds)) return true;
        if (!FMath::IsFinite(TimeoutSeconds)
            || TimeoutSeconds < PinWrightNiagara::MinCompileWaitTimeoutSeconds
            || TimeoutSeconds > PinWrightNiagara::MaxCompileWaitTimeoutSeconds)
        {
            Ctx.SendError(TEXT("INVALID_PARAMS"),
                FString::Printf(TEXT("timeoutSeconds must be between %.2f and %.0f."),
                    PinWrightNiagara::MinCompileWaitTimeoutSeconds,
                    PinWrightNiagara::MaxCompileWaitTimeoutSeconds));
            return true;
        }
    }

    // Resolve asset: try UNiagaraSystem first, then UNiagaraEmitter.
    UObject* Asset = nullptr;
    UNiagaraSystem* System = LoadObject<UNiagaraSystem>(nullptr, *AssetPath);
    UNiagaraEmitter* Emitter = nullptr;
    FString AssetKind;

    if (System)
    {
        Asset = System;
        AssetKind = TEXT("NiagaraSystem");
    }
    else
    {
        Emitter = LoadObject<UNiagaraEmitter>(nullptr, *AssetPath);
        if (Emitter)
        {
            Asset = Emitter;
            AssetKind = TEXT("NiagaraEmitter");
        }
    }

    if (!Asset)
    {
        Ctx.SendError(TEXT("ASSET_NOT_FOUND"),
            FString::Printf(TEXT("Could not load Niagara asset '%s'."), *AssetPath));
        return true;
    }

    // Build minimal resolved target for the shared compile-request path.
    FNiagaraResolvedTarget Target;
    Target.Asset     = Asset;
    Target.System    = System;
    Target.Emitter   = Emitter;
    Target.AssetPath = AssetPath;
    Target.AssetKind = AssetKind;

    // A compile rebuilds each system script's rapid-iteration store from that script's own graph
    // traversal, then overwrites it from the emitter-stage stores it depends on. Any value
    // written straight into a system-script store — the only store niagara.set_parameter can
    // address for an emitter-scoped module constant — is therefore discarded and replaced by the
    // emitter-side value, silently, on every compile. Snapshot the values first and merge them
    // back onto the parameters that survive, so a compile no longer costs authored data.
    PinWrightNiagara::FRapidIterationValueSnapshot RapidIterationSnapshot;
    if (System)
    {
        RapidIterationSnapshot.Capture(*System);
    }

    const double StartTime = FPlatformTime::Seconds();
    const bool bRequested = NiagaraEdit::RequestNiagaraCompile(Target, bForce);

    // The whole point of this verb. UNiagaraSystem::RequestCompile is asynchronous, so the
    // response used to publish `compiled: true` beside `status: "requested"` about 10 ms after
    // issuing a compile whose engine-side completion landed anywhere from 0.08 s to 50 s later.
    // Every downstream verification — spawn_actor, activate_niagara, advance_simulation, a
    // capture — then ran against the PREVIOUS compiled scripts while the caller believed the
    // edit had applied.
    PinWrightNiagara::FCompileWaitOutcome Wait;
    if (bWait && System)
    {
        Wait = PinWrightNiagara::WaitForSystemCompile(*System, bRequested, TimeoutSeconds);
    }
    else if (bWait && Emitter)
    {
        Wait = PinWrightNiagara::WaitForEmitterCompile(
            *Emitter,
            NiagaraEdit::ResolveEmitterVersionGuid(Target),
            Target.CompileRequestSystems,
            TimeoutSeconds);
    }
    else if (System)
    {
        Wait.bOutstanding = System->HasOutstandingCompilationRequests(/*bIncludingGPUShaders=*/false);
    }
    else if (Emitter)
    {
        Wait.bOutstanding = PinWrightNiagara::HasOutstandingEmitterCompile(
            *Emitter,
            NiagaraEdit::ResolveEmitterVersionGuid(Target));
    }

    // After the wait, and ONLY when a wait actually ran. The rebuild is not always synchronous with
    // the request: with rapid-iteration parameters baked out, the prepare is deferred into
    // FNiagaraPrepareCompileTask::Tick after the DDC search, so a merge on a wait:false call runs
    // BEFORE the rebuild it is meant to undo and restores nothing while reporting a number the
    // docs define as "the compile reproduced every value". Reporting the merge as not attempted is
    // the honest result; the caller polls niagara.compile_status and re-runs with wait:true if it
    // needs the guarantee.
    const bool bMergeAttempted = System != nullptr && bRequested && Wait.bWaited;
    int32 RapidIterationPreserved = 0;
    if (bMergeAttempted)
    {
        // Restoring values is a real asset mutation, so it belongs in a transaction like every
        // other mutating path; an empty one is cancelled rather than pushed onto the undo stack.
        FScopedTransaction Transaction(FText::FromString(TEXT("MCP: niagara.compile (rapid-iteration merge)")));
        RapidIterationPreserved = RapidIterationSnapshot.MergeBack();
        if (RapidIterationPreserved == 0)
        {
            Transaction.Cancel();
        }
    }
    const TCHAR* RapidIterationMerge =
        bMergeAttempted            ? TEXT("merged")
        : !System                  ? TEXT("skipped_emitter_asset")
        : !bRequested              ? TEXT("skipped_no_compile")
                                   : TEXT("skipped_no_wait");

    const double DurationMs = (FPlatformTime::Seconds() - StartTime) * 1000.0;

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("assetPath"), AssetPath);
    Result->SetStringField(TEXT("assetKind"), AssetKind);
    // `requested` and `compiled` are deliberately separate fields: one is what was asked of the
    // engine, the other is what was observed to finish. They used to be the same value.
    Result->SetBoolField(TEXT("requested"), bRequested);
    const bool bCompleted = PinWrightNiagara::DidCompileLand(bRequested, Wait);
    Result->SetBoolField(TEXT("compiled"), bCompleted);
    Result->SetBoolField(TEXT("completed"), bCompleted);
    Result->SetBoolField(TEXT("waited"), Wait.bWaited);
    Result->SetNumberField(TEXT("waitedMs"), Wait.WaitedSeconds * 1000.0);
    Result->SetNumberField(TEXT("timeoutSeconds"), TimeoutSeconds);
    Result->SetBoolField(TEXT("timedOut"), Wait.bTimedOut);
    Result->SetArrayField(TEXT("stillCompiling"), EmitStringArray(Wait.StillCompiling));
    Result->SetBoolField(TEXT("outstandingCompilationRequests"), Wait.bOutstanding);
    Result->SetStringField(TEXT("status"), PinWrightNiagara::DescribeCompileOutcome(bRequested, Wait));
    // How many rapid-iteration values the rebuild changed and this call put back. Zero means the
    // compile reproduced every value it found; null means the merge did not run at all, and
    // rapidIterationMerge names why — the two must not be confused, so the count is never
    // published for a call that never measured it.
    if (bMergeAttempted)
    {
        Result->SetNumberField(TEXT("rapidIterationPreserved"), RapidIterationPreserved);
    }
    else
    {
        Result->SetField(TEXT("rapidIterationPreserved"), MakeShared<FJsonValueNull>());
    }
    Result->SetStringField(TEXT("rapidIterationMerge"), RapidIterationMerge);
    Result->SetNumberField(TEXT("durationMs"), DurationMs);

    Ctx.SendSuccess(Result);
    return true;
}

// ---- niagara.compile_status ----
REGISTER_RPC_HANDLER("niagara.compile_status", "niagara",
    "Return a compact, non-blocking compile state for a Niagara system or emitter asset.",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath", "path", "Asset path of the Niagara system or emitter to inspect")
    ))
{
    FString AssetPath;
    if (!Ctx.RequireString(TEXT("assetPath"), AssetPath)) return true;

    UNiagaraSystem* System = LoadObject<UNiagaraSystem>(nullptr, *AssetPath);
    UNiagaraEmitter* Emitter = nullptr;
    FString AssetKind;
    TSharedPtr<FJsonObject> Compile;
    FString CompileQueueScope;
    bool bCompileQueueObserved = false;
    bool bCpuScriptCompilationPending = false;
    bool bCompilationPending = false;
    bool bHasGpuSimulation = false;
    int32 AffectedSystemCount = 0;

    if (System)
    {
        AssetKind = TEXT("NiagaraSystem");
        CompileQueueScope = TEXT("system");
        bCompileQueueObserved = true;
        AffectedSystemCount = 1;
        Compile = NiagaraDumpBuilder::BuildCompileDiagnosticsJson(System);
        bCpuScriptCompilationPending =
            System->HasOutstandingCompilationRequests(/*bIncludingGPUShaders=*/false);
        bCompilationPending =
            System->HasOutstandingCompilationRequests(/*bIncludingGPUShaders=*/true);
        bHasGpuSimulation = PinWrightNiagara::HasGpuComputeSimulation(*System);
    }
    else
    {
        Emitter = LoadObject<UNiagaraEmitter>(nullptr, *AssetPath);
        if (Emitter)
        {
            AssetKind = TEXT("NiagaraEmitter");
            CompileQueueScope = TEXT("loadedSystemsUsingEmitter");
            Compile = NiagaraDumpBuilder::BuildEmitterCompileDiagnosticsJson(Emitter);
            const PinWrightNiagara::FEmitterCompileObservation Observation =
                PinWrightNiagara::ObserveEmitterCompiles(
                    *Emitter,
                    Emitter->GetExposedVersion().VersionGuid);
            AffectedSystemCount = Observation.AffectedSystemCount;
            bCompileQueueObserved = AffectedSystemCount > 0;
            bCpuScriptCompilationPending = Observation.bCpuScriptCompilationPending;
            bCompilationPending = Observation.bAnyCompilationPending;
            bHasGpuSimulation = Observation.bIncludesGpuComputeSimulation;
        }
    }

    if (!System && !Emitter)
    {
        Ctx.SendError(TEXT("ASSET_NOT_FOUND"),
            FString::Printf(TEXT("Could not load Niagara system or emitter '%s'."), *AssetPath));
        return true;
    }

    const PinWrightNiagara::FCompileVerdict Verdict =
        PinWrightNiagara::ReadCompileVerdict(Compile);
    // The diagnostic block is authoritative for system VM/GPU work. OR it into the direct queue
    // observation so stale per-script statuses can never publish a terminal result while either
    // diagnostic pending flag is true.
    bCompilationPending |= Verdict.bPendingCompile;
    const PinWrightNiagara::FCompileProbeVerdict Probe =
        PinWrightNiagara::SummarizeCompileProbe(
            Verdict,
            bCompilationPending,
            bCompileQueueObserved);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("assetPath"), AssetPath);
    Result->SetStringField(TEXT("assetKind"), AssetKind);
    Result->SetStringField(TEXT("status"), Probe.Status);
    Result->SetBoolField(TEXT("completed"), Probe.bCompleted);
    Result->SetBoolField(TEXT("successful"), Probe.bSuccessful);
    Result->SetBoolField(TEXT("outstandingCompilationRequests"), bCompilationPending);
    // Two different questions, so two fields. The queue check above genuinely runs GPU-inclusive
    // on every asset, which is what this states; it says nothing about whether such work exists.
    // Reading it as pending GPU work is what got a CPUSim-only system blamed for a refusal it had
    // nothing to do with, so the measurement it was mistaken for is published beside it.
    Result->SetBoolField(TEXT("outstandingIncludesGpuShaders"), true);
    Result->SetBoolField(TEXT("hasGpuSimulation"), bHasGpuSimulation);
    Result->SetBoolField(TEXT("cpuScriptCompilationPending"), bCpuScriptCompilationPending);
    Result->SetStringField(TEXT("compileQueueScope"), CompileQueueScope);
    Result->SetBoolField(TEXT("compileQueueObserved"), bCompileQueueObserved);
    Result->SetNumberField(TEXT("affectedSystemCount"), AffectedSystemCount);
    Result->SetStringField(TEXT("scriptCompileCheck"),
        PinWrightNiagara::ScriptCompileCheckToString(Verdict.Check));
    Result->SetNumberField(TEXT("failedScriptCount"), Verdict.FailedScripts.Num());
    Ctx.SendSuccess(Result);
    return true;
}
