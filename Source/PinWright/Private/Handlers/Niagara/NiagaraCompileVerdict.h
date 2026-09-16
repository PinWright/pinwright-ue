// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

namespace PinWrightNiagara
{
    // One script whose last compile failed, identified well enough for a caller to open it.
    struct FScriptCompileFailure
    {
        // "system" / "emitter" / "emitterAsset" / "scriptAsset", as the compile block spells it.
        FString OwnerKind;
        // System name, emitter handle name, or standalone asset name.
        FString OwnerName;
        // Slot label: SystemSpawnScript, ParticleUpdateScript, ParticleEventScript:<event>, ...
        FString ScriptUsage;
        // Object path of the UNiagaraScript, empty when the slot carries no script object.
        FString ScriptPath;
        // Wire spelling of the reported ENiagaraScriptCompileStatus (always NCS_Error today).
        FString CompileStatus;
        // Compiler diagnostics for this script, in the order the translator emitted them. Can be
        // empty: a failure whose messages did not survive the session is still a failure.
        TArray<FString> Errors;
    };

    // Whether the scripts of an asset were checked, and what the check found. Deliberately three
    // states and not a bool: "nothing has compiled this asset in this session" is not a pass, and
    // reporting it as one is the class of defect this reader exists to close.
    enum class EScriptCompileCheck : uint8
    {
        // Every script reported a terminal successful compile status.
        Passed,
        // At least one script reported NCS_Error. The engine will refuse to instance the system.
        Failed,
        // At least one script did not report a terminal status (NCS_Unknown / NCS_Dirty /
        // malformed / empty), or the block carried no scripts. Not a verdict - known-good
        // siblings do not turn an unknown or stale script into a pass.
        Unverified
    };

    // What one compile block says about an asset's runnability.
    struct FCompileVerdict
    {
        EScriptCompileCheck Check = EScriptCompileCheck::Unverified;
        // A compile was in flight when the block was built, so every status in it describes the
        // PREVIOUS compile. Only meaningful for a system: the flags come from
        // UNiagaraSystem::HasOutstandingCompilationRequests / HasActiveCompilations, and there is
        // no emitter- or script-level equivalent.
        bool bPendingCompile = false;
        // False when the block carried no pending flags at all, so bPendingCompile is "not
        // measured" rather than "measured false".
        bool bPendingCompileKnown = false;
        TArray<FScriptCompileFailure> FailedScripts;
    };

    // Reads the compile block NiagaraDumpBuilder::Build*CompileDiagnosticsJson produced.
    //
    // Deliberately reads the published JSON rather than re-walking the UNiagaraSystem: the whole
    // defect this closes is a response whose nested block said the scripts had failed while its
    // top-level verdict said the asset was healthy, so the verdict is computed from the same bytes
    // the caller is handed and the two cannot disagree. It also inherits the block's script
    // enumeration for free, which is the only place that knows about event-handler, simulation-
    // stage and GPU scripts.
    //
    // A null or malformed block yields Unverified with no failures.
    FCompileVerdict ReadCompileVerdict(const TSharedPtr<FJsonObject>& Compile);

    // Stable wire spelling for EScriptCompileCheck: "passed" / "failed" / "unverified".
    const TCHAR* ScriptCompileCheckToString(EScriptCompileCheck Check);

    struct FCompileProbeVerdict
    {
        const TCHAR* Status = TEXT("unverified");
        bool bCompleted = false;
        bool bSuccessful = false;
    };

    // Fold script results and queue state into niagara.compile_status's terminal contract.
    // A pending VM/GPU compile always wins over stale script statuses, and an emitter whose
    // affected-system queue could not be observed cannot claim completion.
    FCompileProbeVerdict SummarizeCompileProbe(
        const FCompileVerdict& ScriptVerdict,
        bool bCompilationPending,
        bool bCompileQueueObserved);

    // Caller-facing one-liner naming the script and, when they survived, its compiler
    // diagnostics. Used as the validation issue's message.
    FString DescribeScriptCompileFailure(const FScriptCompileFailure& Failure);
}
