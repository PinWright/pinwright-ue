// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/Niagara/NiagaraDataInterfaceConsistency.h"

#include "NiagaraCommon.h"
#include "NiagaraDataInterface.h"
#include "NiagaraEmitter.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraScript.h"
#include "NiagaraSystem.h"
#include "UObject/UnrealType.h"

// The resolved data-interface set lives in FNiagaraScriptRuntimeCompiledData; on engine versions
// without that header there is nothing to compare against and every check reports Unverified.
#if __has_include("NiagaraScriptRuntimeCompiledData.h")
#include "NiagaraScriptRuntimeCompiledData.h"
// UNiagaraSystem::ForEachScriptWithOwningContext is a template declared in NiagaraSystem.h and
// defined here; it enumerates exactly the scripts the resolve step keys its map by.
#include "NiagaraSystemImpl.h"
#define PINWRIGHT_HAS_NIAGARA_RESOLVED_DI 1
#else
#define PINWRIGHT_HAS_NIAGARA_RESOLVED_DI 0
#endif

namespace PinWrightNiagara
{
#if PINWRIGHT_HAS_NIAGARA_RESOLVED_DI
namespace
{
    // UNiagaraSystem::ScriptRuntimeCompiledDataForEditor is private and its only reader,
    // CreateScriptRuntimeCompiledDataHandle, is not exported. The public
    // UNiagaraSystem::GetScriptRuntimeData is no substitute: it returns null both for a
    // count mismatch and for a script that simply has no resolved set yet, and it caches
    // that null - so it cannot tell the fault from the benign case. Reach the reference
    // object reflectively instead; if the field is ever renamed we report Unverified rather
    // than guess.
    //
    // Returns a mutable pointer even from a const system: the reflection getter hands back a bare
    // UObject* regardless, and RemoveOrphanResolvedDataInterfaces needs to write through it. The
    // read-only callers below bind the result to a const pointer.
    UNiagaraScriptRuntimeCompiledDataEditorReference* FindRuntimeCompiledDataReference(const UNiagaraSystem& System)
    {
        const FObjectPropertyBase* RefProperty = CastField<FObjectPropertyBase>(
            System.GetClass()->FindPropertyByName(TEXT("ScriptRuntimeCompiledDataForEditor")));
        if (!RefProperty)
        {
            return nullptr;
        }

        UObject* Ref = RefProperty->GetObjectPropertyValue_InContainer(&System);
        return Cast<UNiagaraScriptRuntimeCompiledDataEditorReference>(Ref);
    }

    // Indices into RuntimeCompiledData.ResolvedDataInterfaces whose CompileName has no counterpart
    // left in the script's compiled DataInterfaceInfo list, in ascending order.
    //
    // The compiled side is consumed as a multiset rather than a set: two compiled entries sharing a
    // name must license two resolved entries, and a third resolved copy of that name is a leftover
    // like any other. Matching on CompileName is what makes this comparable at all - the resolve
    // step rewrites the resolved entry's Name (prefixing "__INTERNAL__." for anything it could not
    // bind) while leaving CompileName as the compile emitted it.
    void CollectOrphanIndices(
        const UNiagaraScript& Script,
        const FNiagaraScriptRuntimeCompiledData& RuntimeCompiledData,
        TArray<int32>& OutIndices)
    {
        OutIndices.Reset();

        TMap<FName, int32> CompiledRemaining;
        for (const FNiagaraScriptDataInterfaceCompileInfo& Compiled : Script.GetVMExecutableData().DataInterfaceInfo)
        {
            ++CompiledRemaining.FindOrAdd(Compiled.Name);
        }

        for (int32 Index = 0; Index < RuntimeCompiledData.ResolvedDataInterfaces.Num(); ++Index)
        {
            int32* Remaining = CompiledRemaining.Find(RuntimeCompiledData.ResolvedDataInterfaces[Index].CompileName);
            if (Remaining && *Remaining > 0)
            {
                --(*Remaining);
                continue;
            }
            OutIndices.Add(Index);
        }
    }

    void DescribeOrphan(
        const UNiagaraScript& Script,
        const FNiagaraEmitterHandle* OwningHandle,
        const FNiagaraScriptRuntimeCompiledData& RuntimeCompiledData,
        int32 ResolvedIndex,
        FOrphanResolvedDataInterface& OutEntry)
    {
        const FNiagaraScriptResolvedDataInterfaceInfo& Resolved = RuntimeCompiledData.ResolvedDataInterfaces[ResolvedIndex];
        OutEntry.ScriptPath = Script.GetPathName();
        OutEntry.EmitterName = OwningHandle ? OwningHandle->GetName().ToString() : FString();
        OutEntry.ResolvedIndex = ResolvedIndex;
        OutEntry.Name = Resolved.Name.ToString();
        OutEntry.CompileName = Resolved.CompileName.ToString();
        OutEntry.TypeName = Resolved.ParameterStoreVariable.GetType().GetName();
        OutEntry.DataInterfaceClass = Resolved.ResolvedDataInterface
            ? Resolved.ResolvedDataInterface->GetClass()->GetPathName()
            : FString();
        OutEntry.bIsInternal = Resolved.bIsInternal;
    }

    // FNiagaraResolvedUserDataInterfaceBinding::ScriptParameterStoreDataInterfaceIndex is a
    // POSITION in ResolvedDataInterfaces, so removing entries silently re-points every binding
    // above them. Left unremapped, a User. data-interface parameter would drive whichever
    // interface slid into its old slot - a wrong bind with no error anywhere, which is the class
    // of fault this repair exists to end rather than relocate.
    //
    // RemovedIndices are positions in the array BEFORE any removal, ascending; call this with the
    // same list used to prune, in either order relative to the pruning.
    void RemapUserDataInterfaceBindings(
        const TArray<int32>& RemovedIndices,
        TArray<FNiagaraResolvedUserDataInterfaceBinding>& Bindings)
    {
        for (int32 Cursor = Bindings.Num() - 1; Cursor >= 0; --Cursor)
        {
            const int32 Target = Bindings[Cursor].ScriptParameterStoreDataInterfaceIndex;
            if (RemovedIndices.Contains(Target))
            {
                // The entry this binding pointed at is gone; the binding has nothing left to mean.
                Bindings.RemoveAt(Cursor);
                continue;
            }

            int32 Shift = 0;
            for (const int32 Removed : RemovedIndices)
            {
                if (Removed < Target)
                {
                    ++Shift;
                }
            }
            Bindings[Cursor].ScriptParameterStoreDataInterfaceIndex = Target - Shift;
        }
    }

    // FNiagaraVMExecutableData::IsValid() is `LastCompileStatus != NCS_Unknown`, and
    // UNiagaraScript::InvalidateCompileResults resets the whole struct - so this is the engine's
    // own discriminator between "this script owns no data interfaces" and "this script's compiled
    // results were thrown away and its list reads empty for that reason".
    bool ScriptCarriesCompiledResults(const UNiagaraScript& Script)
    {
        return Script.GetVMExecutableData().IsValid();
    }

    // The one place the three-way verdict is decided, so CheckDataInterfaceCounts and
    // FindOrphanResolvedDataInterfaces cannot drift into two readings of the same asset.
    EDataInterfaceConsistency ClassifyComparison(
        const FDataInterfaceComparisonCounts& Counts,
        bool bAnyCountMismatch)
    {
        if (Counts.ComparedScripts == 0)
        {
            return EDataInterfaceConsistency::Unverified;
        }
        if (bAnyCountMismatch)
        {
            return EDataInterfaceConsistency::Mismatched;
        }
        if (Counts.ScriptsWithCompiledResults == 0)
        {
            return EDataInterfaceConsistency::Unverified;
        }
        return EDataInterfaceConsistency::Consistent;
    }
}
#endif

EDataInterfaceConsistency CheckDataInterfaceCounts(
    const UNiagaraSystem& System,
    TArray<FDataInterfaceCountMismatch>& OutMismatches,
    FDataInterfaceComparisonCounts* OutCounts)
{
    OutMismatches.Reset();
    if (OutCounts)
    {
        *OutCounts = FDataInterfaceComparisonCounts();
    }

#if PINWRIGHT_HAS_NIAGARA_RESOLVED_DI
    const UNiagaraScriptRuntimeCompiledDataEditorReference* Ref = FindRuntimeCompiledDataReference(System);
    if (!Ref)
    {
        return EDataInterfaceConsistency::Unverified;
    }

    FDataInterfaceComparisonCounts Counts;
    System.ForEachScriptWithOwningContext(
        [&Counts, &OutMismatches, Ref](const UNiagaraSystem*, const FNiagaraEmitterHandle* OwningHandle, UNiagaraScript* Script)
        {
            if (!Script)
            {
                return;
            }

            const FNiagaraScriptRuntimeCompiledData* RuntimeCompiledData =
                Ref->ScriptRuntimeCompiledDataMap.Find(FNiagaraScriptDataKey(OwningHandle, *Script));
            if (!RuntimeCompiledData)
            {
                // No resolved set for this script - e.g. an emitter handle added since the last
                // compile, or a GPU script on a CPU-sim emitter. Nothing to compare.
                return;
            }

            ++Counts.ComparedScripts;
            if (ScriptCarriesCompiledResults(*Script))
            {
                ++Counts.ScriptsWithCompiledResults;
            }

            const int32 CompiledCount = Script->GetVMExecutableData().DataInterfaceInfo.Num();
            const int32 ResolvedCount = RuntimeCompiledData->ResolvedDataInterfaces.Num();
            if (CompiledCount == ResolvedCount)
            {
                return;
            }

            FDataInterfaceCountMismatch& Mismatch = OutMismatches.AddDefaulted_GetRef();
            Mismatch.ScriptPath = Script->GetPathName();
            Mismatch.EmitterName = OwningHandle ? OwningHandle->GetName().ToString() : FString();
            Mismatch.CompiledCount = CompiledCount;
            Mismatch.ResolvedCount = ResolvedCount;
        });

    if (OutCounts)
    {
        *OutCounts = Counts;
    }
    return ClassifyComparison(Counts, OutMismatches.Num() > 0);
#else
    return EDataInterfaceConsistency::Unverified;
#endif
}

bool DidWriteArmDataInterfaceMismatch(
    EDataInterfaceConsistency Before,
    EDataInterfaceConsistency After)
{
    return Before == EDataInterfaceConsistency::Consistent
        && After == EDataInterfaceConsistency::Mismatched;
}

FString DescribeDataInterfaceMismatches(const TArray<FDataInterfaceCountMismatch>& Mismatches)
{
    TArray<FString> Parts;
    Parts.Reserve(Mismatches.Num());
    for (const FDataInterfaceCountMismatch& Mismatch : Mismatches)
    {
        Parts.Add(FString::Printf(TEXT("%s (compiled %d, resolved %d)"),
            *Mismatch.ScriptPath, Mismatch.CompiledCount, Mismatch.ResolvedCount));
    }
    return FString::Join(Parts, TEXT("; "));
}

const TCHAR* DataInterfaceConsistencyToString(EDataInterfaceConsistency Consistency)
{
    switch (Consistency)
    {
    case EDataInterfaceConsistency::Consistent:
        return TEXT("consistent");
    case EDataInterfaceConsistency::Mismatched:
        return TEXT("mismatched");
    default:
        return TEXT("unverified");
    }
}

EDataInterfaceConsistency FindOrphanResolvedDataInterfaces(
    const UNiagaraSystem& System,
    TArray<FOrphanResolvedDataInterface>& OutOrphans)
{
    OutOrphans.Reset();

#if PINWRIGHT_HAS_NIAGARA_RESOLVED_DI
    const UNiagaraScriptRuntimeCompiledDataEditorReference* Ref = FindRuntimeCompiledDataReference(System);
    if (!Ref)
    {
        return EDataInterfaceConsistency::Unverified;
    }

    FDataInterfaceComparisonCounts Counts;
    bool bAnyCountMismatch = false;
    System.ForEachScriptWithOwningContext(
        [&Counts, &bAnyCountMismatch, &OutOrphans, Ref](const UNiagaraSystem*, const FNiagaraEmitterHandle* OwningHandle, UNiagaraScript* Script)
        {
            if (!Script)
            {
                return;
            }

            const FNiagaraScriptRuntimeCompiledData* RuntimeCompiledData =
                Ref->ScriptRuntimeCompiledDataMap.Find(FNiagaraScriptDataKey(OwningHandle, *Script));
            if (!RuntimeCompiledData)
            {
                // Same skip as CheckDataInterfaceCounts: no resolved set means nothing to compare,
                // not an empty one.
                return;
            }

            ++Counts.ComparedScripts;
            if (ScriptCarriesCompiledResults(*Script))
            {
                ++Counts.ScriptsWithCompiledResults;
            }
            if (Script->GetVMExecutableData().DataInterfaceInfo.Num() != RuntimeCompiledData->ResolvedDataInterfaces.Num())
            {
                bAnyCountMismatch = true;
            }

            TArray<int32> OrphanIndices;
            CollectOrphanIndices(*Script, *RuntimeCompiledData, OrphanIndices);
            for (const int32 Index : OrphanIndices)
            {
                DescribeOrphan(*Script, OwningHandle, *RuntimeCompiledData, Index,
                    OutOrphans.AddDefaulted_GetRef());
            }
        });

    // The verdict is deliberately the COUNT verdict rather than "were any orphans found". The two
    // answer different questions and the caller needs both: a Consistent system that still reports
    // orphans is name-level drift removal must not touch, and calling that Mismatched would
    // contradict every other verb's reading of the same asset.
    return ClassifyComparison(Counts, bAnyCountMismatch);
#else
    return EDataInterfaceConsistency::Unverified;
#endif
}

EOrphanRemovalOutcome RemoveOrphanResolvedDataInterfaces(
    UNiagaraSystem& System,
    FOrphanRemovalReport& OutReport)
{
    OutReport = FOrphanRemovalReport();

#if PINWRIGHT_HAS_NIAGARA_RESOLVED_DI
    UNiagaraScriptRuntimeCompiledDataEditorReference* Ref = FindRuntimeCompiledDataReference(System);
    if (!Ref)
    {
        return EOrphanRemovalOutcome::Unverified;
    }

    // Plan every script before writing to any of them. A half-applied repair leaves some scripts
    // reconciled and others not, and no single verdict then describes the asset - the caller would
    // have to unpick which half landed.
    struct FScriptPlan
    {
        UNiagaraScript* Script = nullptr;
        FNiagaraScriptRuntimeCompiledData* RuntimeCompiledData = nullptr;
        const FNiagaraEmitterHandle* OwningHandle = nullptr;
        TArray<int32> OrphanIndices;
    };

    TArray<FScriptPlan> Plans;
    int32 ComparedScripts = 0;
    System.ForEachScriptWithOwningContext(
        [&ComparedScripts, &Plans, &OutReport, Ref](const UNiagaraSystem*, const FNiagaraEmitterHandle* OwningHandle, UNiagaraScript* Script)
        {
            if (!Script)
            {
                return;
            }

            FNiagaraScriptRuntimeCompiledData* RuntimeCompiledData =
                Ref->ScriptRuntimeCompiledDataMap.Find(FNiagaraScriptDataKey(OwningHandle, *Script));
            if (!RuntimeCompiledData)
            {
                return;
            }

            ++ComparedScripts;

            TArray<int32> OrphanIndices;
            CollectOrphanIndices(*Script, *RuntimeCompiledData, OrphanIndices);

            const int32 CompiledCount = Script->GetVMExecutableData().DataInterfaceInfo.Num();
            const int32 ResolvedCount = RuntimeCompiledData->ResolvedDataInterfaces.Num();
            if (ResolvedCount - OrphanIndices.Num() != CompiledCount)
            {
                // Two shapes land here and neither is repairable by deletion. Either the compiled
                // list is the longer one, so nothing removed can close the gap; or the two lists
                // agree in COUNT while disagreeing by name, meaning one leftover stands in for one
                // entry the bytecode does want - dropping it would convert a system that resolves
                // the wrong interface into one that asserts on its next tick.
                FDataInterfaceCountMismatch& Blocked = OutReport.Irreconcilable.AddDefaulted_GetRef();
                Blocked.ScriptPath = Script->GetPathName();
                Blocked.EmitterName = OwningHandle ? OwningHandle->GetName().ToString() : FString();
                Blocked.CompiledCount = CompiledCount;
                Blocked.ResolvedCount = ResolvedCount;
                return;
            }

            if (OrphanIndices.Num() == 0)
            {
                return;
            }

            FScriptPlan& Plan = Plans.AddDefaulted_GetRef();
            Plan.Script = Script;
            Plan.RuntimeCompiledData = RuntimeCompiledData;
            Plan.OwningHandle = OwningHandle;
            Plan.OrphanIndices = MoveTemp(OrphanIndices);
        });

    if (ComparedScripts == 0)
    {
        return EOrphanRemovalOutcome::Unverified;
    }
    if (OutReport.Irreconcilable.Num() > 0)
    {
        return EOrphanRemovalOutcome::RefusedIrreconcilable;
    }

    TArray<FDataInterfaceCountMismatch> ScratchMismatches;
    if (Plans.Num() == 0)
    {
        OutReport.VerdictAfter = CheckDataInterfaceCounts(System, ScratchMismatches);
        return EOrphanRemovalOutcome::NothingToRemove;
    }

    System.Modify();
    for (FScriptPlan& Plan : Plans)
    {
        // Describe before the indices shift; afterwards ResolvedIndex would name a different entry.
        for (const int32 Index : Plan.OrphanIndices)
        {
            DescribeOrphan(*Plan.Script, Plan.OwningHandle, *Plan.RuntimeCompiledData, Index,
                OutReport.Removed.AddDefaulted_GetRef());
        }

        Plan.Script->Modify();

        // UNiagaraScript::CachedDefaultDataInterfaces is the serialized list the next resolve pass
        // rebuilds the resolved set from, one cached entry per resolved entry in the same order.
        // Prune it only while that correspondence is observable: if the two lengths already
        // disagree, this code cannot say which cached entry produced which resolved one, and a
        // guess would delete a data interface the bytecode still uses. The resolved-set pruning
        // still lands in that case, and the caller is told the durable half did not.
        TArray<FNiagaraScriptDataInterfaceInfo>& CachedDefaults = Plan.Script->GetCachedDefaultDataInterfaces();
        const bool bCachedDefaultsAligned =
            CachedDefaults.Num() == Plan.RuntimeCompiledData->ResolvedDataInterfaces.Num();

        // Descending, so every lower index stays addressable as the array shrinks.
        for (int32 Cursor = Plan.OrphanIndices.Num() - 1; Cursor >= 0; --Cursor)
        {
            const int32 Index = Plan.OrphanIndices[Cursor];
            Plan.RuntimeCompiledData->ResolvedDataInterfaces.RemoveAt(Index);
            if (bCachedDefaultsAligned)
            {
                CachedDefaults.RemoveAt(Index);
            }
        }

        RemapUserDataInterfaceBindings(Plan.OrphanIndices, Plan.RuntimeCompiledData->ResolvedUserDataInterfaceBindings);

        ++OutReport.ScriptsTouched;
        if (bCachedDefaultsAligned)
        {
            ++OutReport.ScriptsPrunedDurably;
        }
    }

    // Every FNiagaraScriptRuntimeData the system cached was derived from the sets just rewritten -
    // including the NULL entry UNiagaraSystem::GetScriptRuntimeData memoises for exactly the
    // scripts that were mismatched and then returns forever without re-deriving. Without this the
    // asset would be repaired and still behave as though it were not.
    System.OnCompiledDataInterfaceChanged();

    // Re-measure rather than infer. The removal counts say what this code did; only the check says
    // whether the asset is now sound.
    OutReport.VerdictAfter = CheckDataInterfaceCounts(System, ScratchMismatches);
    return EOrphanRemovalOutcome::Removed;
#else
    return EOrphanRemovalOutcome::Unverified;
#endif
}

const TCHAR* OrphanRemovalOutcomeToString(EOrphanRemovalOutcome Outcome)
{
    switch (Outcome)
    {
    case EOrphanRemovalOutcome::NothingToRemove:
        return TEXT("nothing_to_remove");
    case EOrphanRemovalOutcome::Removed:
        return TEXT("removed");
    case EOrphanRemovalOutcome::RefusedIrreconcilable:
        return TEXT("refused_irreconcilable");
    default:
        return TEXT("unverified");
    }
}
}
