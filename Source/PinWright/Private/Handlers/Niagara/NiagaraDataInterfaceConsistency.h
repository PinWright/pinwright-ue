// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"

class UNiagaraSystem;

namespace PinWrightNiagara
{
    // One script whose compiled data-interface list disagrees with the resolved one the
    // system holds for it.
    struct FDataInterfaceCountMismatch
    {
        // Object path of the offending UNiagaraScript.
        FString ScriptPath;
        // Owning emitter handle name, empty for the system spawn/update scripts.
        FString EmitterName;
        // FNiagaraVMExecutableData::DataInterfaceInfo.Num() - what the bytecode was built against.
        int32 CompiledCount = 0;
        // FNiagaraScriptRuntimeCompiledData::ResolvedDataInterfaces.Num() - what the exec context
        // would be populated from.
        int32 ResolvedCount = 0;
    };

    enum class EDataInterfaceConsistency : uint8
    {
        // At least one script carries compiled results, and every script that has a resolved
        // data-interface set agrees with its compiled list. The only verdict that is a pass.
        Consistent,
        // At least one script disagrees. The system is fatal on its next tick.
        Mismatched,
        // Nothing MEANINGFUL could be compared: the system has no resolved data-interface set
        // (never compiled in this session), this engine version does not expose one, or every
        // script that had one has had its compiled results invalidated so the comparison is
        // 0-against-0 and vacuous. Not a verdict - callers must not report it as a pass.
        Unverified
    };

    // Why a verdict came out the way it did: the two population counts behind it. Published so a
    // caller can tell a pass over N real comparisons from the vacuous 0-against-0 shape, and so a
    // handler can say which of the two an `unverified` is.
    struct FDataInterfaceComparisonCounts
    {
        // Scripts that had a resolved data-interface set to compare against at all.
        int32 ComparedScripts = 0;
        // Of those, the ones whose compiled results are still present. A script whose compile was
        // invalidated has an empty FNiagaraVMExecutableData, so its compiled data-interface list
        // reads 0 for a reason that has nothing to do with the script owning no data interfaces -
        // comparing that 0 against a resolved 0 produces an equality that means nothing.
        int32 ScriptsWithCompiledResults = 0;
    };

    // Compares, for every script the system owns, the compiled data-interface count against the
    // resolved one. This is the same comparison the engine runs in
    // FNiagaraScriptRuntimeCompiledData::ValidateWithScript during presave, except that the
    // engine only logs a Warning and invalidates the compile results - the system keeps ticking
    // with the mismatched execution context and asserts
    // (`DataSetIdx < ExecCtx->DataSets.Num()`, VectorVMRuntime.cpp) on a concurrent worker the
    // next time anything ticks it. That assert is an appError, so it takes the editor process
    // down, minutes after and unrelated to the write that caused it.
    //
    // Scripts with no resolved entry are skipped rather than flagged: an emitter handle added
    // since the last compile legitimately has none yet, and a missing entry is not evidence of a
    // mismatch. OutMismatches is reset on entry and only filled for Mismatched.
    //
    // A pass requires at least one script that still carries compiled results. Without that the
    // equality being reported is 0-compiled against 0-resolved on scripts whose bytecode was
    // thrown away, which is what an emitter-scoped write leaves behind - the state that USED to be
    // published as `consistent` and is indistinguishable, on the wire, from a system that really
    // was compared (B-niagara-set-parameter-emitter-scope-arms-di-mismatch).
    //
    // OutCounts, when supplied, receives the two population counts behind the verdict.
    EDataInterfaceConsistency CheckDataInterfaceCounts(
        const UNiagaraSystem& System,
        TArray<FDataInterfaceCountMismatch>& OutMismatches,
        FDataInterfaceComparisonCounts* OutCounts = nullptr);

    // True when the write that moved a system from Before to After is the one that armed the
    // VectorVM assert: the system passed a real comparison and does not any more. Before ==
    // Mismatched means the caller inherited the state and is not its author; Before == Unverified
    // means nothing was measurable beforehand, and claiming authorship from that would be a guess.
    bool DidWriteArmDataInterfaceMismatch(
        EDataInterfaceConsistency Before,
        EDataInterfaceConsistency After);

    // Single-line, caller-facing summary naming each offending script and both counts.
    FString DescribeDataInterfaceMismatches(const TArray<FDataInterfaceCountMismatch>& Mismatches);

    // Stable wire spelling for EDataInterfaceConsistency: "consistent" / "mismatched" /
    // "unverified". Handlers echo this so a caller can tell a verified pass from an unverified one.
    const TCHAR* DataInterfaceConsistencyToString(EDataInterfaceConsistency Consistency);

    // ------------------------------------------------------------------------------------------
    // Orphan resolved data interfaces: the entries CheckDataInterfaceCounts counts but does not
    // name.
    //
    // A script's resolved set is built one-for-one from UNiagaraScript::CachedDefaultDataInterfaces
    // (FNiagaraResolveDIHelpers::ResolveDIsForScript appends exactly one resolved entry per cached
    // default, in order; the later ResolveInternalDataInterfaces pass only rewrites entries in
    // place). Each cached default carries the compiled entry's name in CompileName, so a resolved
    // entry whose CompileName has no counterpart left in
    // FNiagaraVMExecutableData::DataInterfaceInfo is a leftover the bytecode no longer references -
    // the state a compile normally clears and that survives when the script's compiled results are
    // adopted from elsewhere (UNiagaraScript::SynchronizeExecutablesWithCompilation copies the
    // cached list wholesale).
    // ------------------------------------------------------------------------------------------

    // One resolved data-interface entry with no compiled counterpart, identified well enough for a
    // caller to recognise it in the asset and for the removal pass to address it.
    struct FOrphanResolvedDataInterface
    {
        // Object path of the owning UNiagaraScript.
        FString ScriptPath;
        // Owning emitter handle name, empty for the system spawn/update scripts.
        FString EmitterName;
        // Position in that script's FNiagaraScriptRuntimeCompiledData::ResolvedDataInterfaces.
        int32 ResolvedIndex = INDEX_NONE;
        // Resolved name. Carries the "__INTERNAL__." prefix when the resolve step could not bind
        // the entry to a parameter, which is the usual shape of a leftover.
        FString Name;
        // Name as the compile emitted it - the key matched against the compiled list.
        FString CompileName;
        // FNiagaraTypeDefinition name of the parameter-store variable the entry resolved to.
        FString TypeName;
        // Class path of the resolved UNiagaraDataInterface, empty when the entry resolved to none.
        FString DataInterfaceClass;
        bool bIsInternal = false;
    };

    // Enumerates every resolved data-interface entry with no compiled counterpart, across every
    // script of System that has a resolved set. OutOrphans is reset on entry.
    //
    // The return value is the same verdict CheckDataInterfaceCounts would give, and carries the
    // same warning: on Unverified nothing could be compared, so an empty OutOrphans is not
    // evidence that there are none. A Consistent system can still report orphans - that is the
    // count-equal case where one leftover displaced one real entry, and it is exactly the case
    // RemoveOrphanResolvedDataInterfaces refuses to act on.
    EDataInterfaceConsistency FindOrphanResolvedDataInterfaces(
        const UNiagaraSystem& System,
        TArray<FOrphanResolvedDataInterface>& OutOrphans);

    enum class EOrphanRemovalOutcome : uint8
    {
        // Nothing could be compared (see EDataInterfaceConsistency::Unverified). Not a pass, and
        // nothing was mutated.
        Unverified,
        // Every script that has a resolved set is free of orphans. Nothing was mutated. This is a
        // success, not an error: the verb has to be idempotent, so a repeat of a repair that
        // already landed must converge rather than fail.
        NothingToRemove,
        // Orphans were removed. See FOrphanRemovalReport for what went and for the re-measured
        // verdict.
        Removed,
        // At least one script's orphan set does not reconcile its two counts, so removing it would
        // leave (or create) a mismatch. NOTHING was mutated, on any script; Irreconcilable names
        // the offenders. Recompiling the system is the route out, not this verb.
        RefusedIrreconcilable
    };

    struct FOrphanRemovalReport
    {
        // The entries actually dropped from the resolved sets.
        TArray<FOrphanResolvedDataInterface> Removed;
        // Scripts whose resolved set was edited.
        int32 ScriptsTouched = 0;
        // Of those, the ones whose serialized UNiagaraScript::CachedDefaultDataInterfaces list was
        // pruned alongside. That list is what the next resolve pass rebuilds the resolved set
        // from, so a script missing from this count regrows its orphans on the system's next
        // compile - reported rather than assumed, because the one-for-one correspondence the
        // pruning relies on is an engine detail this code verifies per script instead of trusting.
        int32 ScriptsPrunedDurably = 0;
        // CheckDataInterfaceCounts re-run after the removal. This is the measurement that says
        // whether the repair worked; the removal counts alone cannot.
        EDataInterfaceConsistency VerdictAfter = EDataInterfaceConsistency::Unverified;
        // Only filled for RefusedIrreconcilable.
        TArray<FDataInterfaceCountMismatch> Irreconcilable;
    };

    // Drops every orphan resolved data interface from System, and prunes the cached default entry
    // each one was built from so the next resolve does not put it back.
    //
    // Planned in full before anything is written: unless dropping a script's orphans brings its
    // resolved count back to its compiled count, the whole call is refused with
    // RefusedIrreconcilable and no script is touched. Callers must quiesce the system's live
    // instances first (PinWrightNiagara::KillSystemInstances) - a ticking FNiagaraSystemInstance
    // reads the resolved set this rewrites.
    //
    // OutReport is reset on entry. The caller owns dirtying and saving.
    EOrphanRemovalOutcome RemoveOrphanResolvedDataInterfaces(
        UNiagaraSystem& System,
        FOrphanRemovalReport& OutReport);

    // Stable wire spelling for EOrphanRemovalOutcome: "unverified" / "nothing_to_remove" /
    // "removed" / "refused_irreconcilable".
    const TCHAR* OrphanRemovalOutcomeToString(EOrphanRemovalOutcome Outcome);
}
