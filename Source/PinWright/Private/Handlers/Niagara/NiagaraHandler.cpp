// Copyright (c) 2026 Alexander Penkin. MIT License.

// NiagaraHandler.cpp - Migrated from PinWright_NiagaraHandlers.cpp
// Niagara system/emitter creation, actor spawning, parameter modification, and ribbon creation

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/PackagePathCompose.h"
#include "PinWrightHelpers.h"
#include "PinWrightSubsystem.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "Async/Async.h"
#include "Engine/World.h"
#include "Modules/ModuleManager.h"
#include "NiagaraActor.h"
#include "NiagaraComponent.h"
#include "NiagaraEmitter.h"
#include "NiagaraEmitterFactoryNew.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraParameterCollection.h"
#include "NiagaraSystem.h"
#include "NiagaraSystemFactoryNew.h"
#include "ScopedTransaction.h"
#include "UObject/Package.h"
#include "Handlers/Niagara/NiagaraCompileWait.h"
#include "Handlers/Niagara/NiagaraDataInterfaceConsistency.h"
#include "Handlers/Niagara/NiagaraEditTypes.h"
#include "Handlers/Niagara/NiagaraEditorOpenGuard.h"
#include "Handlers/Niagara/NiagaraGraphResetUtils.h"
#include "Handlers/Niagara/NiagaraInstanceUtils.h"
#include "Utils/AssetUtils.h"
#include "Utils/JsonBuilders.h"
#if __has_include("Subsystems/EditorActorSubsystem.h")
#include "Subsystems/EditorActorSubsystem.h"
#elif __has_include("EditorActorSubsystem.h")
#include "EditorActorSubsystem.h"
#endif
#if __has_include("ViewModels/Stack/NiagaraStackGraphUtilities.h")
#include "ViewModels/Stack/NiagaraStackGraphUtilities.h"
#endif

namespace
{
    // Post-write validity gate shared by the two system-mutating verbs below.
    //
    // A UNiagaraSystem whose compiled data-interface list disagrees with its resolved one is
    // fatal on its NEXT tick: the bytecode indexes a data set the execution context never
    // allocated and VectorVM asserts on a concurrent worker
    // (`DataSetIdx < ExecCtx->DataSets.Num()`), which is an appError and takes the whole editor
    // process down. The engine's own mitigation - a LogNiagara Warning plus
    // InvalidateCompileResults during presave - is not enough, because the system keeps ticking.
    //
    // The fault is therefore latent: the write that leaves the system in this state returns
    // clean, and the kill lands minutes later from an unrelated caller that merely forces a
    // re-tick. So this write path must neither report success on a system in that state nor
    // persist it to disk.
    //
    // The same measurement taken BEFORE a mutation, so the response can attribute the state it
    // reports. Without it the three handle verbs could only echo the verdict they ended in, and a
    // caller had no way to tell a system it inherited broken from one this call broke.
    PinWrightNiagara::EDataInterfaceConsistency MeasureDataInterfaceVerdict(const UNiagaraSystem& System)
    {
        TArray<PinWrightNiagara::FDataInterfaceCountMismatch> Scratch;
        return PinWrightNiagara::CheckDataInterfaceCounts(System, Scratch);
    }

    // Returns true when the caller may proceed. On a mismatch it sends the error itself,
    // carrying the mutation facts so the caller still learns what landed in memory, and
    // returns false. OutVerdict always receives the verdict, including Unverified - a pass this
    // check could not actually confirm must not be echoed as one it could. BeforeVerdict is the
    // same measurement taken ahead of the mutation; it turns the refusal payload's
    // `dataInterfaceDelta` into an attribution rather than a restatement.
    bool RejectOnDataInterfaceMismatch(
        FHandlerContext& Ctx,
        UNiagaraSystem& System,
        const FString& SystemPath,
        const TSharedPtr<FJsonObject>& MutationFacts,
        PinWrightNiagara::EDataInterfaceConsistency BeforeVerdict,
        PinWrightNiagara::EDataInterfaceConsistency& OutVerdict)
    {
        TArray<PinWrightNiagara::FDataInterfaceCountMismatch> Mismatches;
        const PinWrightNiagara::EDataInterfaceConsistency Consistency =
            PinWrightNiagara::CheckDataInterfaceCounts(System, Mismatches);
        OutVerdict = Consistency;
        if (Consistency != PinWrightNiagara::EDataInterfaceConsistency::Mismatched)
        {
            return true;
        }

        TSharedPtr<FJsonObject> ErrorData = MutationFacts.IsValid() ? MutationFacts : MakeShared<FJsonObject>();
        ErrorData->SetStringField(TEXT("systemPath"), SystemPath);
        ErrorData->SetStringField(TEXT("dataInterfaceCheck"),
            PinWrightNiagara::DataInterfaceConsistencyToString(Consistency));
        NiagaraEdit::AddDataInterfaceDelta(ErrorData, /*bBeforeMeasured=*/true, BeforeVerdict, Consistency);
        ErrorData->SetBoolField(TEXT("saved"), false);

        TArray<TSharedPtr<FJsonValue>> MismatchedScripts;
        MismatchedScripts.Reserve(Mismatches.Num());
        for (const PinWrightNiagara::FDataInterfaceCountMismatch& Mismatch : Mismatches)
        {
            TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
            Entry->SetStringField(TEXT("scriptPath"), Mismatch.ScriptPath);
            Entry->SetStringField(TEXT("emitter"), Mismatch.EmitterName);
            Entry->SetNumberField(TEXT("compiledDataInterfaces"), Mismatch.CompiledCount);
            Entry->SetNumberField(TEXT("resolvedDataInterfaces"), Mismatch.ResolvedCount);
            MismatchedScripts.Add(MakeShared<FJsonValueObject>(Entry));
        }
        ErrorData->SetArrayField(TEXT("mismatchedScripts"), MismatchedScripts);

        Ctx.SendError(TEXT("NIAGARA_DATA_INTERFACE_MISMATCH"),
            FString::Printf(
                TEXT("Niagara system '%s' has scripts whose compiled data-interface count differs from the resolved count: %s. ")
                TEXT("Ticking it asserts inside the VectorVM on a worker thread and kills the editor, so the asset was not saved. ")
                TEXT("Recompile the system, or remove the data interfaces that no longer appear in its compiled scripts."),
                *SystemPath,
                *PinWrightNiagara::DescribeDataInterfaceMismatches(Mismatches)),
            ErrorData);
        return false;
    }

    // Resolves the `emitter` argument the handle-addressing verbs take - a handle Guid or a handle
    // name, matched case-insensitively - against one system. Shared so remove_emitter and
    // refresh_emitter cannot drift into two spellings of the same lookup or two spellings of its
    // two failures (rpc-design.md: two verbs sharing a concept must share its semantics).
    // Returns true when the handle resolved; on failure it has already sent EMITTER_HANDLE_NOT_FOUND
    // or AMBIGUOUS_EMITTER_HANDLE and the caller must return immediately.
    bool ResolveNiagaraEmitterHandle(
        FHandlerContext& Ctx,
        const UNiagaraSystem& System,
        const FString& EmitterArg,
        FGuid& OutId,
        FString& OutName)
    {
        FGuid ParsedGuid;
        if (FGuid::Parse(EmitterArg, ParsedGuid))
        {
            for (const FNiagaraEmitterHandle& Handle : System.GetEmitterHandles())
            {
                if (Handle.GetId() == ParsedGuid)
                {
                    OutId = ParsedGuid;
                    OutName = Handle.GetName().ToString();
                    return true;
                }
            }
            Ctx.SendError(TEXT("EMITTER_HANDLE_NOT_FOUND"),
                FString::Printf(TEXT("No emitter handle with Guid '%s' found in system."), *EmitterArg));
            return false;
        }

        TArray<FGuid> Matches;
        FString MatchedName;
        for (const FNiagaraEmitterHandle& Handle : System.GetEmitterHandles())
        {
            if (Handle.GetName().ToString().Equals(EmitterArg, ESearchCase::IgnoreCase))
            {
                Matches.Add(Handle.GetId());
                MatchedName = Handle.GetName().ToString();
            }
        }
        if (Matches.Num() == 0)
        {
            Ctx.SendError(TEXT("EMITTER_HANDLE_NOT_FOUND"),
                FString::Printf(TEXT("No emitter handle named '%s' found in system."), *EmitterArg));
            return false;
        }
        if (Matches.Num() > 1)
        {
            Ctx.SendError(TEXT("AMBIGUOUS_EMITTER_HANDLE"),
                FString::Printf(TEXT("Multiple emitter handles named '%s' found; use the Guid to disambiguate."), *EmitterArg));
            return false;
        }
        OutId = Matches[0];
        OutName = MatchedName;
        return true;
    }

    // Post-write wiring gate shared by the two system-mutating verbs below.
    //
    // An FNiagaraEmitterHandle does nothing on its own. The system's SystemSpawn / SystemUpdate
    // graph must also carry a UNiagaraNodeEmitter naming that handle, on the chain feeding the
    // system output nodes - that node is what invokes the emitter's scripts. A system whose
    // handle list was changed without rebuilding those nodes compiles, validates strict-clean,
    // saves, lists the emitter everywhere, and never spawns a particle
    // (B-niagara-authored-emitter-forces-inert). Both verbs call
    // PinWrightNiagara::RebuildSystemEmitterNodes; this reads the resulting graph back and
    // refuses to report - or persist - a system whose emitters nothing invokes.
    //
    // The read walks the graph rather than the writer's own bookkeeping, so it can contradict
    // the write. Returns true when the caller may proceed; on failure it sends the error itself,
    // carrying the mutation facts so the caller still learns what landed in memory.
    // OutInvokedEmitters always receives the measured count of handles the graph does invoke, so
    // the success response publishes a measurement rather than a constant.
    bool RejectOnUninvokedEmitter(
        FHandlerContext& Ctx,
        UNiagaraSystem& System,
        const FString& SystemPath,
        int32 RebuiltNodeCount,
        const TSharedPtr<FJsonObject>& MutationFacts,
        int32& OutInvokedEmitters)
    {
        TArray<FName> UninvokedHandles;
        const bool bGraphReadable = PinWrightNiagara::FindUninvokedEmitterHandles(System, UninvokedHandles);
        OutInvokedEmitters = bGraphReadable ? (System.GetEmitterHandles().Num() - UninvokedHandles.Num()) : 0;
        if (bGraphReadable && UninvokedHandles.Num() == 0)
        {
            return true;
        }

        TSharedPtr<FJsonObject> ErrorData = MutationFacts.IsValid() ? MutationFacts : MakeShared<FJsonObject>();
        ErrorData->SetStringField(TEXT("systemPath"), SystemPath);
        ErrorData->SetNumberField(TEXT("emitterNodesRebuilt"), RebuiltNodeCount);
        ErrorData->SetNumberField(TEXT("emittersInvokedBySystemGraph"), OutInvokedEmitters);
        ErrorData->SetBoolField(TEXT("systemGraphReadable"), bGraphReadable);
        ErrorData->SetBoolField(TEXT("saved"), false);

        FString Detail;
        if (!bGraphReadable)
        {
            Detail = TEXT("its SystemSpawn/SystemUpdate graph could not be read, so no emitter node could be placed in it");
        }
        else
        {
            TArray<TSharedPtr<FJsonValue>> UninvokedJson;
            TArray<FString> UninvokedNames;
            UninvokedJson.Reserve(UninvokedHandles.Num());
            UninvokedNames.Reserve(UninvokedHandles.Num());
            for (const FName& HandleName : UninvokedHandles)
            {
                UninvokedJson.Add(MakeShared<FJsonValueString>(HandleName.ToString()));
                UninvokedNames.Add(HandleName.ToString());
            }
            ErrorData->SetArrayField(TEXT("uninvokedEmitters"), UninvokedJson);
            Detail = FString::Printf(TEXT("the system graph does not invoke %s"), *FString::Join(UninvokedNames, TEXT(", ")));
        }

        Ctx.SendError(TEXT("NIAGARA_EMITTER_NOT_IN_SYSTEM_GRAPH"),
            FString::Printf(
                TEXT("Niagara system '%s' has emitter handles that can never run: %s. ")
                TEXT("Such an emitter compiles clean, validates clean and spawns nothing, so the asset was not saved. ")
                TEXT("Rebuild the system from a complete stock system with asset.duplicate rather than shipping this one."),
                *SystemPath,
                *Detail),
            ErrorData);
        return false;
    }
}

// ---- niagara.add_emitter ----
REGISTER_RPC_HANDLER("niagara.add_emitter", "niagara", "Add one existing UNiagaraEmitter asset to a UNiagaraSystem as an inherited child of that asset.",
    RPC_PARAMS(
        RPC_PARAM_REQ("systemPath", "path", "Asset path of the Niagara system"),
        RPC_PARAM_REQ("emitterPath", "path", "Asset path of the Niagara emitter to add"),
        RPC_PARAM_OPT("name", "string", "Emitter handle name inside the system. Defaults to the emitter asset name."),
        RPC_PARAM_OPT("inherit", "boolean", "Keep the system's copy linked to the emitter asset as its parent, so later edits to the asset can be merged in with niagara.refresh_emitter. Defaults true. false takes an unlinked snapshot instead - the editor's 'Remove Parent Emitter' - which no later edit of the asset can ever reach."),
        RPC_PARAM_OPT("compile", "boolean", "Compile the system after mutation. Defaults false."),
        RPC_PARAM_OPT("save", "boolean", "Save the system asset after mutation. Defaults false.")
    ))
{
    FString SystemPath;
    if (!Ctx.RequireString(TEXT("systemPath"), SystemPath)) return true;

    FString EmitterPath;
    if (!Ctx.RequireString(TEXT("emitterPath"), EmitterPath)) return true;

    UNiagaraSystem* System = LoadObject<UNiagaraSystem>(nullptr, *SystemPath);
    if (!System)
    {
        Ctx.SendError(TEXT("ASSET_NOT_FOUND"), FString::Printf(TEXT("Could not load Niagara system '%s'."), *SystemPath));
        return true;
    }

    // AddEmitterHandle below reshapes System->GetEmitterHandles(). An open FNiagaraSystemToolkit
    // still points at the old handles and faults on the next Slate redraw, so refuse instead.
    if (PinWrightNiagara::RejectStructuralEditWhileAssetEditorOpen(Ctx, System, SystemPath))
    {
        return true;
    }

    UNiagaraEmitter* Emitter = LoadObject<UNiagaraEmitter>(nullptr, *EmitterPath);
    if (!Emitter)
    {
        Ctx.SendError(TEXT("ASSET_NOT_FOUND"), FString::Printf(TEXT("Could not load Niagara emitter '%s'."), *EmitterPath));
        return true;
    }

    const FString RequestedName = Ctx.GetString(TEXT("name"));
    const FName HandleName = RequestedName.IsEmpty() ? FName(*Emitter->GetName()) : FName(*RequestedName);
    const bool bCompileRequested = Ctx.GetBool(TEXT("compile"), false);
    const bool bSaveRequested = Ctx.GetBool(TEXT("save"), false);
    // Default true, matching what the Niagara editor's Add Emitter does with an ordinary emitter
    // asset: the system gets a CHILD of the asset, not a photocopy of it. See the measurement
    // below for why this is read back rather than assumed.
    const bool bInheritRequested = Ctx.GetBool(TEXT("inherit"), true);
    const FGuid EmitterVersion = Emitter->GetExposedVersion().VersionGuid;
    FVersionedNiagaraEmitterData* SourceEmitterData = Emitter->GetEmitterData(EmitterVersion);
    if (!SourceEmitterData)
    {
        Ctx.SendError(TEXT("EMITTER_DATA_MISSING"), TEXT("Source Niagara emitter has no versioned emitter data."));
        return true;
    }
    if (!SourceEmitterData->GraphSource)
    {
        Ctx.SendError(TEXT("EMITTER_GRAPH_SOURCE_MISSING"), TEXT("Source Niagara emitter has no graph source and cannot be added safely."));
        return true;
    }

    // Measured before anything is written, so the response can attribute the verdict it ends with.
    const PinWrightNiagara::EDataInterfaceConsistency DataInterfaceVerdictBefore =
        MeasureDataInterfaceVerdict(*System);

    FNiagaraEmitterHandle NewHandle;
    int32 RebuiltEmitterNodes = INDEX_NONE;
    // Measured off the copy the engine made, never echoed from the request. Set true only when the
    // system's emitter really points back at `Emitter`.
    bool bInherited = false;
    // Set when inherit was asked for, the engine refused to link, and the handle was taken back
    // out again. The error is sent after the transaction closes.
    bool bNotInheritable = false;
    {
        const FScopedTransaction Transaction(NSLOCTEXT("PinWright", "NiagaraAddEmitter", "Add Niagara Emitter"));
        // Quiesce before the reshape, not only before the compile below - appending is not the
        // safe half of the pair. A live FNiagaraEmitterInstance caches its POSITION in the handle
        // array (FNiagaraSystemInstance::InitEmitters passes EmitterIdx to EmitterInstance->Init)
        // and reads the handle back through it unchecked: `Sys->GetEmitterHandles()[EmitterIndex]`
        // (NiagaraEmitterInstance.cpp:104-108). UNiagaraSystem::EmitterHandles is a TArray, so
        // AddEmitterHandle's `EmitterHandles.Add` can REALLOCATE it and free the buffer that
        // reference points into. Indices 0..N-1 surviving the append is no reprieve either: the
        // system-side execution order is resized from EmitterHandles.Num() and then used to index
        // the instance's now-shorter Emitters array unchecked in Tick_Concurrent, behind nothing
        // but a checkSlow. That is why the engine's own add path opens with the same call
        // under the comment "Kill all system instances before modifying the emitter handle list to
        // prevent accessing deleted data" (FNiagaraEditorUtilities::AddEmitterToSystem,
        // NiagaraEditorUtilities.cpp:2141) - word for word what its removal counterpart says. The
        // rebuild below widens it further: it destroys and recreates every UNiagaraNodeEmitter in
        // the system graph, which a live instance's compiled scripts are running against.
        PinWrightNiagara::KillSystemInstances(*System);
        System->Modify();
        NewHandle = System->AddEmitterHandle(*Emitter, HandleName, EmitterVersion);

        // AddEmitterHandle always DUPLICATES the source emitter into the system - the handle never
        // points at the asset itself. What varies, and what this reads back, is whether the copy
        // keeps a link to the asset it came from: UNiagaraEmitter::CreateWithParentAndOwner sets
        // VersionedParent + VersionedParentAtLastMerge on the copy, and AddEmitterHandle then
        // strips them again when the source asset declares itself non-inheritable
        // (NiagaraSystem.cpp:3019-3032). With the link the system tracks the asset and
        // MergeChangesFromParent - niagara.refresh_emitter - carries later edits across; without
        // it the handle is frozen at this instant forever, which is the defect this measurement
        // exists to name (B-niagara-add-emitter-snapshots-emitter-silently: three encounters, one
        // of which shipped an effect missing the size ramp its emitter asset carried the whole
        // time, past a clean compile, a clean save and a clean strict validate).
        //
        // Read back rather than re-derived: the engine spells the inheritability rule differently
        // on different versions (bIsInheritable on 5.4+, TemplateSpecification on 5.3), and a
        // duplicated rule would drift silently. The copy's own parent pointer cannot.
        FVersionedNiagaraEmitterData* AddedData = NewHandle.GetEmitterData();
        UNiagaraEmitter* AddedEmitter = NewHandle.GetInstance().Emitter;
        bInherited = AddedData != nullptr && AddedData->GetParent().Emitter == Emitter;

        if (bInheritRequested && !bInherited)
        {
            // Asked for a child, got a photocopy. Refusing is the point: the pre-fix verb returned
            // this case as an ordinary success and the caller learned about it, if at all, weeks
            // later from a structural read. Take the handle back out so nothing half-written
            // survives - the graph has not been rebuilt yet on this path, so the handle list is
            // the only thing to undo, and this is the same call niagara.remove_emitter makes.
            TSet<FGuid> ToRemove;
            ToRemove.Add(NewHandle.GetId());
            System->RemoveEmitterHandlesById(ToRemove);
            bNotInheritable = true;
        }
        else
        {
            if (!bInheritRequested && bInherited && AddedData && AddedEmitter)
            {
                // The editor's "Remove Parent Emitter", verbatim: Modify() then RemoveParent()
                // (FNiagaraEmitterViewModel::RemoveParentEmitter, NiagaraEmitterViewModel.cpp:245).
                AddedEmitter->Modify();
                AddedData->RemoveParent();
                bInherited = false;
            }
            // AddEmitterHandle only extends the handle list. Without the matching UNiagaraNodeEmitter
            // pair in the system graph nothing ever calls the emitter's scripts, and every signal the
            // caller can read still says the system is healthy - so rebuild them here, before the
            // compile below bakes the graph.
            RebuiltEmitterNodes = PinWrightNiagara::RebuildSystemEmitterNodes(*System);
            System->MarkPackageDirty();
        }
    }

    if (bNotInheritable)
    {
        TSharedPtr<FJsonObject> ErrorData = MakeShared<FJsonObject>();
        ErrorData->SetStringField(TEXT("systemPath"), SystemPath);
        ErrorData->SetStringField(TEXT("emitterPath"), EmitterPath);
        ErrorData->SetBoolField(TEXT("inheritRequested"), true);
        ErrorData->SetStringField(TEXT("emitterSource"), TEXT("snapshot"));
        ErrorData->SetBoolField(TEXT("handleAdded"), false);
        ErrorData->SetNumberField(TEXT("emitterCount"), System->GetEmitterHandles().Num());
        Ctx.SendError(TEXT("EMITTER_NOT_INHERITABLE"), FString::Printf(
            TEXT("Niagara emitter '%s' declares itself non-inheritable, so the copy '%s' would have taken of it "
                 "carries no link back to the asset and no later edit of the asset could ever reach it. ")
            TEXT("Nothing was added. Stock Niagara template and behaviour-example emitters ship this way, and ")
            TEXT("asset.duplicate preserves it - the editor's own emitter wizard clears it on the asset it creates. ")
            TEXT("Either set bIsInheritable on the emitter asset and retry, or pass inherit:false to take the ")
            TEXT("snapshot deliberately (its content is then frozen at add time; refreshing it needs ")
            TEXT("niagara.remove_emitter + niagara.add_emitter, which discards any edit made to the system's own copy)."),
            *EmitterPath,
            *SystemPath),
            ErrorData);
        return true;
    }

    // Read the graph back and refuse before compiling or saving: an inert emitter must not be
    // baked into compiled scripts or written to disk under a success response.
    int32 InvokedEmitters = 0;
    {
        TSharedPtr<FJsonObject> WiringFacts = MakeShared<FJsonObject>();
        WiringFacts->SetStringField(TEXT("operation"), TEXT("add_emitter"));
        WiringFacts->SetStringField(TEXT("emitterPath"), EmitterPath);
        WiringFacts->SetStringField(TEXT("emitterName"), NewHandle.GetName().ToString());
        WiringFacts->SetStringField(TEXT("emitterId"), NewHandle.GetId().ToString());
        WiringFacts->SetNumberField(TEXT("emitterCount"), System->GetEmitterHandles().Num());
        WiringFacts->SetStringField(TEXT("emitterSource"), bInherited ? TEXT("inherited") : TEXT("snapshot"));
        WiringFacts->SetBoolField(TEXT("compileRequested"), bCompileRequested);
        WiringFacts->SetBoolField(TEXT("compiled"), false);
        WiringFacts->SetBoolField(TEXT("saveRequested"), bSaveRequested);
        if (!RejectOnUninvokedEmitter(Ctx, *System, SystemPath, RebuiltEmitterNodes, WiringFacts, InvokedEmitters))
        {
            return true;
        }
    }

    bool bCompiled = false;
    PinWrightNiagara::FCompileWaitOutcome CompileWait;
    if (bCompileRequested)
    {
        // A recompile swaps the compiled scripts out from under any FNiagaraSystemInstance still
        // ticking this system on a worker thread; the new bytecode then indexes data sets the old
        // exec context never allocated and the VectorVM asserts, killing the editor process.
        // This is a second kill, and deliberately so: the one above covers the handle reshape,
        // this one states the requirement where the bytecode swap is. Nothing between them can
        // re-activate a component - the handler holds the game thread throughout - so the repeat
        // costs one TObjectIterator sweep and keeps each mutation's guard readable on its own.
        PinWrightNiagara::KillSystemInstances(*System);
        const bool bCompileIssued = System->RequestCompile(true);
        // RequestCompile is asynchronous, and both the data-interface check and the save below
        // read compiled state. Run either while the compile is in flight and presave serialises
        // 0 compiled data interfaces against N resolved ones — the exact state that kills the
        // editor on the next tick. Waiting here is what makes the check below a measurement
        // rather than a coin flip, and `compiled` an observation rather than an echo.
        CompileWait = PinWrightNiagara::WaitForSystemCompile(*System, bCompileIssued);
        bCompiled = PinWrightNiagara::DidCompileLand(bCompileIssued, CompileWait);
    }

    // Gate the save and the success response on the post-write data-interface invariant.
    PinWrightNiagara::EDataInterfaceConsistency DataInterfaceVerdict =
        PinWrightNiagara::EDataInterfaceConsistency::Unverified;
    {
        TSharedPtr<FJsonObject> MutationFacts = MakeShared<FJsonObject>();
        MutationFacts->SetStringField(TEXT("operation"), TEXT("add_emitter"));
        MutationFacts->SetStringField(TEXT("emitterPath"), EmitterPath);
        MutationFacts->SetStringField(TEXT("emitterName"), NewHandle.GetName().ToString());
        MutationFacts->SetStringField(TEXT("emitterId"), NewHandle.GetId().ToString());
        MutationFacts->SetNumberField(TEXT("emitterCount"), System->GetEmitterHandles().Num());
        MutationFacts->SetStringField(TEXT("emitterSource"), bInherited ? TEXT("inherited") : TEXT("snapshot"));
        MutationFacts->SetBoolField(TEXT("compileRequested"), bCompileRequested);
        MutationFacts->SetBoolField(TEXT("compiled"), bCompiled);
        MutationFacts->SetBoolField(TEXT("saveRequested"), bSaveRequested);
        if (!RejectOnDataInterfaceMismatch(Ctx, *System, SystemPath, MutationFacts,
                DataInterfaceVerdictBefore, DataInterfaceVerdict))
        {
            return true;
        }
    }

    bool bSaved = false;
    EAssetSaveState SaveState = EAssetSaveState::NotRequested;
    // A compile that has not landed must not be persisted: that is the write which puts the
    // invalidated compile on disk. `saved:false` beside `saveRequested:true` is the honest
    // report; re-run niagara.compile, then asset.save.
    const bool bCompileAllowsSave = !bCompileRequested
        || (bCompiled && PinWrightNiagara::MayPersistAfterCompileWait(CompileWait));
    if (bSaveRequested && bCompileAllowsSave)
    {
        bSaved = SaveAssetToDiskReportingPresence(
            System, /*bForce=*/true, nullptr, nullptr, &SaveState);
    }
    else if (bSaveRequested)
    {
        SaveState = EAssetSaveState::Failed;
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetBoolField(TEXT("success"), true);
    Result->SetStringField(TEXT("operation"), TEXT("add_emitter"));
    Result->SetStringField(TEXT("systemPath"), SystemPath);
    Result->SetStringField(TEXT("emitterPath"), EmitterPath);
    Result->SetStringField(TEXT("emitterName"), NewHandle.GetName().ToString());
    Result->SetStringField(TEXT("emitterId"), NewHandle.GetId().ToString());
    Result->SetNumberField(TEXT("emitterCount"), System->GetEmitterHandles().Num());
    // The relationship the write created, read off the copy's own parent pointer. `inherited`
    // means the system tracks `emitterPath` and niagara.refresh_emitter can carry later edits
    // across; `snapshot` (only reachable by asking for inherit:false) means the copy is frozen
    // here forever and nothing done to the asset afterwards will ever show up in this system.
    // Published on every success because the pre-fix verb published nothing that distinguished
    // the two, and three sessions shipped stale systems believing they had a reference.
    Result->SetStringField(TEXT("emitterSource"), bInherited ? TEXT("inherited") : TEXT("snapshot"));
    if (bInherited)
    {
        Result->SetStringField(TEXT("parentEmitterPath"), Emitter->GetPathName());
    }
    // Measured off the system graph after the write, not echoed from the request: this is the
    // pair that separates an emitter that will run from one that will not. The gate above makes
    // them equal on success, so a caller can compare them without knowing that.
    Result->SetNumberField(TEXT("emittersInvokedBySystemGraph"), InvokedEmitters);
    Result->SetNumberField(TEXT("emitterNodesRebuilt"), RebuiltEmitterNodes);
    Result->SetBoolField(TEXT("compileRequested"), bCompileRequested);
    Result->SetBoolField(TEXT("compiled"), bCompiled);
    NiagaraEdit::AddNiagaraAssetSaveReport(Result, bSaveRequested, bSaved, SaveState);
    Result->SetStringField(TEXT("dataInterfaceCheck"),
        PinWrightNiagara::DataInterfaceConsistencyToString(DataInterfaceVerdict));
    NiagaraEdit::AddDataInterfaceDelta(Result, /*bBeforeMeasured=*/true,
        DataInterfaceVerdictBefore, DataInterfaceVerdict);
    AddAssetVerification(Result, System);
    Ctx.SendSuccess(Result);
    return true;
}

// ---- niagara.remove_emitter ----
REGISTER_RPC_HANDLER("niagara.remove_emitter", "niagara", "Remove an emitter handle from a UNiagaraSystem by Guid or name.",
    RPC_PARAMS(
        RPC_PARAM_REQ("systemPath", "path", "Asset path of the Niagara system"),
        RPC_PARAM_REQ("emitter", "string", "Emitter handle Guid or name to remove"),
        RPC_PARAM_OPT("compile", "bool", "Compile the system after removal. Defaults false."),
        RPC_PARAM_OPT("save", "bool", "Save the system asset after removal. Defaults false.")
    ))
{
    FString SystemPath;
    if (!Ctx.RequireString(TEXT("systemPath"), SystemPath)) return true;

    FString EmitterArg;
    if (!Ctx.RequireString(TEXT("emitter"), EmitterArg)) return true;

    UNiagaraSystem* System = LoadObject<UNiagaraSystem>(nullptr, *SystemPath);
    if (!System)
    {
        Ctx.SendError(TEXT("ASSET_NOT_FOUND"), FString::Printf(TEXT("Could not load Niagara system '%s'."), *SystemPath));
        return true;
    }

    // RemoveEmitterHandlesById below reshapes System->GetEmitterHandles(); same open-toolkit
    // stale-handle redraw crash as add_emitter, so the same refusal.
    if (PinWrightNiagara::RejectStructuralEditWhileAssetEditorOpen(Ctx, System, SystemPath))
    {
        return true;
    }

    // Resolve emitter handle: Guid first, then name. Shared with niagara.refresh_emitter.
    FGuid ResolvedId;
    FString ResolvedName;
    if (!ResolveNiagaraEmitterHandle(Ctx, *System, EmitterArg, ResolvedId, ResolvedName))
    {
        return true;
    }

    // Measured before anything is written, so the response can attribute the verdict it ends with.
    const PinWrightNiagara::EDataInterfaceConsistency DataInterfaceVerdictBefore =
        MeasureDataInterfaceVerdict(*System);

    int32 RebuiltEmitterNodes = INDEX_NONE;
    {
        const FScopedTransaction Transaction(NSLOCTEXT("PinWright", "NiagaraRemoveEmitter", "Remove Niagara Emitter"));
        // Load-bearing, for the same reason spelled out at add_emitter above and one step worse
        // here: removal SHIFTS the positions live FNiagaraEmitterInstances cached, so
        // `Sys->GetEmitterHandles()[EmitterIndex]` reads the wrong handle or runs off the end of
        // a shorter array. The engine quiesces identically for both verbs - compare
        // FNiagaraEditorUtilities::RemoveEmittersFromSystemByEmitterHandleId
        // (NiagaraEditorUtilities.cpp:2199) with AddEmitterToSystem above it; neither
        // AddEmitterHandle nor RemoveEmitterHandlesById stops anything itself, so the kill is
        // always the caller's to make.
        PinWrightNiagara::KillSystemInstances(*System);
        System->Modify();
        TSet<FGuid> ToRemove;
        ToRemove.Add(ResolvedId);
        System->RemoveEmitterHandlesById(ToRemove);
        // RemoveEmitterHandlesById only shortens the handle list; the removed handle's
        // UNiagaraNodeEmitter pair would stay in the system graph naming an id that no longer
        // resolves. The rebuild drops those and re-splices the survivors.
        RebuiltEmitterNodes = PinWrightNiagara::RebuildSystemEmitterNodes(*System);
        System->MarkPackageDirty();
    }

    const bool bCompileRequested = Ctx.GetBool(TEXT("compile"), false);
    const bool bSaveRequested = Ctx.GetBool(TEXT("save"), false);

    // Same pre-compile, pre-save gate as add_emitter: a surviving emitter the system graph no
    // longer invokes must not be compiled in or written to disk under a success response.
    int32 InvokedEmitters = 0;
    {
        TSharedPtr<FJsonObject> WiringFacts = MakeShared<FJsonObject>();
        WiringFacts->SetStringField(TEXT("operation"), TEXT("remove_emitter"));
        WiringFacts->SetBoolField(TEXT("removed"), true);
        WiringFacts->SetStringField(TEXT("emitter"), ResolvedName);
        WiringFacts->SetStringField(TEXT("handleId"), ResolvedId.ToString());
        WiringFacts->SetNumberField(TEXT("remainingEmitters"), System->GetEmitterHandles().Num());
        WiringFacts->SetBoolField(TEXT("compileRequested"), bCompileRequested);
        WiringFacts->SetBoolField(TEXT("compiled"), false);
        WiringFacts->SetBoolField(TEXT("saveRequested"), bSaveRequested);
        if (!RejectOnUninvokedEmitter(Ctx, *System, SystemPath, RebuiltEmitterNodes, WiringFacts, InvokedEmitters))
        {
            return true;
        }
    }

    bool bCompiled = false;
    PinWrightNiagara::FCompileWaitOutcome CompileWait;
    if (bCompileRequested)
    {
        const bool bCompileIssued = System->RequestCompile(true);
        // See add_emitter above: RequestCompile is asynchronous, so the data-interface check and
        // the save must not read compiled state while it is still in flight.
        CompileWait = PinWrightNiagara::WaitForSystemCompile(*System, bCompileIssued);
        bCompiled = PinWrightNiagara::DidCompileLand(bCompileIssued, CompileWait);
    }

    // Gate the save and the success response on the post-write data-interface invariant.
    PinWrightNiagara::EDataInterfaceConsistency DataInterfaceVerdict =
        PinWrightNiagara::EDataInterfaceConsistency::Unverified;
    {
        TSharedPtr<FJsonObject> MutationFacts = MakeShared<FJsonObject>();
        MutationFacts->SetStringField(TEXT("operation"), TEXT("remove_emitter"));
        MutationFacts->SetBoolField(TEXT("removed"), true);
        MutationFacts->SetStringField(TEXT("emitter"), ResolvedName);
        MutationFacts->SetStringField(TEXT("handleId"), ResolvedId.ToString());
        MutationFacts->SetNumberField(TEXT("remainingEmitters"), System->GetEmitterHandles().Num());
        MutationFacts->SetBoolField(TEXT("compileRequested"), bCompileRequested);
        MutationFacts->SetBoolField(TEXT("compiled"), bCompiled);
        MutationFacts->SetBoolField(TEXT("saveRequested"), bSaveRequested);
        if (!RejectOnDataInterfaceMismatch(Ctx, *System, SystemPath, MutationFacts,
                DataInterfaceVerdictBefore, DataInterfaceVerdict))
        {
            return true;
        }
    }

    bool bSaved = false;
    EAssetSaveState SaveState = EAssetSaveState::NotRequested;
    // See add_emitter: an unlanded compile must not be written to disk.
    const bool bCompileAllowsSave = !bCompileRequested
        || (bCompiled && PinWrightNiagara::MayPersistAfterCompileWait(CompileWait));
    if (bSaveRequested && bCompileAllowsSave)
    {
        bSaved = SaveAssetToDiskReportingPresence(
            System, /*bForce=*/true, nullptr, nullptr, &SaveState);
    }
    else if (bSaveRequested)
    {
        SaveState = EAssetSaveState::Failed;
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetBoolField(TEXT("removed"), true);
    Result->SetStringField(TEXT("emitter"), ResolvedName);
    Result->SetStringField(TEXT("handleId"), ResolvedId.ToString());
    Result->SetNumberField(TEXT("remainingEmitters"), System->GetEmitterHandles().Num());
    // Measured off the system graph after the write; equal to remainingEmitters on success.
    Result->SetNumberField(TEXT("emittersInvokedBySystemGraph"), InvokedEmitters);
    Result->SetNumberField(TEXT("emitterNodesRebuilt"), RebuiltEmitterNodes);
    Result->SetBoolField(TEXT("compiled"), bCompiled);
    NiagaraEdit::AddNiagaraAssetSaveReport(Result, bSaveRequested, bSaved, SaveState);
    Result->SetStringField(TEXT("dataInterfaceCheck"),
        PinWrightNiagara::DataInterfaceConsistencyToString(DataInterfaceVerdict));
    NiagaraEdit::AddDataInterfaceDelta(Result, /*bBeforeMeasured=*/true,
        DataInterfaceVerdictBefore, DataInterfaceVerdict);
    AddAssetVerification(Result, System);
    Ctx.SendSuccess(Result);
    return true;
}

// ---- niagara.refresh_emitter ----
//
// The non-destructive half of the inheritance story niagara.add_emitter opens. An inherited handle
// holds a copy of the parent emitter plus VersionedParentAtLastMerge, the parent as it stood when
// the two were last reconciled; editing the parent asset does NOT touch the system, so the merge
// runs on load or when something asks for it. Nothing in PinWright asked for it, which is why a
// system could sit stale through a clean compile, a clean save and a clean strict validate
// (B-niagara-add-emitter-snapshots-emitter-silently).
//
// Deliberately a MERGE, not a re-add. remove_emitter + add_emitter also picks up the parent's
// changes, but it throws away every edit made to the system's own copy - the per-handle spawn
// counts, sizes and radii an author sets in the system rather than in the emitter asset. That cost
// is what left one reporter with no non-destructive route mid-iteration. MergeChangesFromParent
// applies the parent's diff on top of those overrides, which is what the Niagara editor itself does
// when it loads a system whose parent has moved on (UNiagaraEmitter::UpdateEmitterAfterLoad).
REGISTER_RPC_HANDLER("niagara.refresh_emitter", "niagara", "Merge changes made to a parent emitter asset into the UNiagaraSystem handles that inherit from it, keeping the system's own overrides.",
    RPC_PARAMS(
        RPC_PARAM_REQ("systemPath", "path", "Asset path of the Niagara system"),
        RPC_PARAM_OPT("emitter", "string", "Emitter handle Guid or name to refresh. Omit to refresh every inherited handle in the system."),
        RPC_PARAM_OPT("compile", "boolean", "Compile the system after merging. Defaults false. A merge changes the emitter's graph, so nothing reaches a running effect until the system is recompiled and saved."),
        RPC_PARAM_OPT("save", "boolean", "Save the system asset after merging. Defaults false.")
    ))
{
    FString SystemPath;
    if (!Ctx.RequireString(TEXT("systemPath"), SystemPath)) return true;

    UNiagaraSystem* System = LoadObject<UNiagaraSystem>(nullptr, *SystemPath);
    if (!System)
    {
        Ctx.SendError(TEXT("ASSET_NOT_FOUND"), FString::Printf(TEXT("Could not load Niagara system '%s'."), *SystemPath));
        return true;
    }

    // A merge replaces the emitter's scripts and graph through UpdateFromMergedCopy. An open
    // FNiagaraSystemToolkit's stack widgets hold the outgoing ones, so the same refusal the two
    // handle-reshaping verbs carry applies here for the same reason.
    if (PinWrightNiagara::RejectStructuralEditWhileAssetEditorOpen(Ctx, System, SystemPath))
    {
        return true;
    }

    const FString EmitterArg = Ctx.GetString(TEXT("emitter"));
    FGuid RequestedId;
    FString RequestedName;
    if (!EmitterArg.IsEmpty() && !ResolveNiagaraEmitterHandle(Ctx, *System, EmitterArg, RequestedId, RequestedName))
    {
        return true;
    }

    const bool bCompileRequested = Ctx.GetBool(TEXT("compile"), false);
    const bool bSaveRequested = Ctx.GetBool(TEXT("save"), false);

    TArray<TSharedPtr<FJsonValue>> Refreshed;
    TArray<TSharedPtr<FJsonValue>> Skipped;
    int32 InheritedHandles = 0;
    int32 MergesApplied = 0;
    int32 MergesFailed = 0;

    // Preflight the selected handles before opening a transaction, quiescing live instances, or
    // calling Modify. A snapshot has no parent to merge and a refused request must leave the
    // system untouched.
    for (const FNiagaraEmitterHandle& Handle : System->GetEmitterHandles())
    {
        if (!EmitterArg.IsEmpty() && Handle.GetId() != RequestedId)
        {
            continue;
        }

        const FString HandleName = Handle.GetName().ToString();
        FVersionedNiagaraEmitterData* Data = Handle.GetEmitterData();
        UNiagaraEmitter* HandleEmitter = Handle.GetInstance().Emitter;
        UNiagaraEmitter* Parent = Data ? Data->GetParent().Emitter : nullptr;
        if (!Data || !HandleEmitter || !Parent)
        {
            TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
            Entry->SetStringField(TEXT("emitter"), HandleName);
            Entry->SetStringField(TEXT("handleId"), Handle.GetId().ToString());
            Entry->SetStringField(TEXT("reason"), TEXT("no_parent"));
            Skipped.Add(MakeShared<FJsonValueObject>(Entry));
            continue;
        }

        ++InheritedHandles;
    }

    if (InheritedHandles == 0)
    {
        TSharedPtr<FJsonObject> ErrorData = MakeShared<FJsonObject>();
        ErrorData->SetStringField(TEXT("systemPath"), SystemPath);
        ErrorData->SetNumberField(TEXT("emitterCount"), System->GetEmitterHandles().Num());
        ErrorData->SetArrayField(TEXT("skipped"), Skipped);

        FString ErrorMessage;
        if (EmitterArg.IsEmpty())
        {
            ErrorMessage = FString::Printf(
                TEXT("Nothing in Niagara system '%s' inherits from an emitter asset, so there is no parent to merge from. ")
                TEXT("A snapshot handle carries no link back to the asset it was copied from; its only route to the asset's ")
                TEXT("current content is niagara.remove_emitter + niagara.add_emitter, which discards every edit made to the ")
                TEXT("system's own copy of that emitter. Read niagara.inspect's emitters[].versionedEmitterData.parent to see ")
                TEXT("which handles are inherited."),
                *SystemPath);
        }
        else
        {
            const FString RequestedIdString = RequestedId.ToString();
            ErrorData->SetStringField(TEXT("emitter"), RequestedName);
            ErrorData->SetStringField(TEXT("handleId"), RequestedIdString);
            ErrorMessage = FString::Printf(
                TEXT("Niagara system '%s' handle '%s' (%s) does not inherit from an emitter asset, so there is no parent to merge from. ")
                TEXT("This snapshot cannot be re-linked non-destructively because the original source identity is absent. ")
                TEXT("The migration path is destructive: preserve or manually reconcile the system-copy overrides first, ")
                TEXT("ensure the source emitter asset is inheritable, then use niagara.remove_emitter + niagara.add_emitter ")
                TEXT("with inherit:true. Read niagara.inspect's emitters[].versionedEmitterData.parent to confirm the handle's relationship."),
                *SystemPath,
                *RequestedName,
                *RequestedIdString);
        }

        Ctx.SendError(TEXT("EMITTER_NOT_INHERITED"), ErrorMessage, ErrorData);
        return true;
    }

    // Measured before anything is written, so the response can attribute the verdict it ends with.
    const PinWrightNiagara::EDataInterfaceConsistency DataInterfaceVerdictBefore =
        MeasureDataInterfaceVerdict(*System);

    // Recount the candidates that reach the merge loop; the preflight count above is the refusal
    // gate, while this count controls the post-write dirtying/reporting path.
    InheritedHandles = 0;
    {
        const FScopedTransaction Transaction(NSLOCTEXT("PinWright", "NiagaraRefreshEmitter", "Refresh Niagara Emitter From Parent"));
        // The merge swaps compiled scripts and graph out from under any FNiagaraSystemInstance
        // still ticking this system, exactly as a recompile does. Same quiesce, same reason.
        PinWrightNiagara::KillSystemInstances(*System);
        System->Modify();

        for (const FNiagaraEmitterHandle& Handle : System->GetEmitterHandles())
        {
            if (!EmitterArg.IsEmpty() && Handle.GetId() != RequestedId)
            {
                continue;
            }

            const FString HandleName = Handle.GetName().ToString();
            FVersionedNiagaraEmitterData* Data = Handle.GetEmitterData();
            UNiagaraEmitter* HandleEmitter = Handle.GetInstance().Emitter;
            UNiagaraEmitter* Parent = nullptr;
            if (Data)
            {
                Parent = Data->GetParent().Emitter;
            }
            if (!Data || !HandleEmitter || !Parent)
            {
                continue;
            }

            ++InheritedHandles;
            // Measured before the merge, so the response can say whether this call did anything
            // rather than only that it ran.
            const bool bSynchronizedBefore = Data->IsSynchronizedWithParent();
            HandleEmitter->Modify();
            const TArray<INiagaraMergeManager::FMergeEmitterResults> Results = HandleEmitter->MergeChangesFromParent();

            bool bAnyFailed = false;
            bool bGraphModified = false;
            TArray<TSharedPtr<FJsonValue>> Errors;
            for (const INiagaraMergeManager::FMergeEmitterResults& MergeResult : Results)
            {
                bGraphModified = bGraphModified || MergeResult.bModifiedGraph;
                if (MergeResult.MergeResult != INiagaraMergeManager::EMergeEmitterResult::SucceededNoDifferences
                    && MergeResult.MergeResult != INiagaraMergeManager::EMergeEmitterResult::SucceededDifferencesApplied)
                {
                    bAnyFailed = true;
                    const FString ErrorText = MergeResult.GetErrorMessagesString();
                    if (!ErrorText.IsEmpty())
                    {
                        Errors.Add(MakeShared<FJsonValueString>(ErrorText));
                    }
                }
            }
            // Read back rather than inferred from the merge's own return: this is the state a
            // later niagara.validate and niagara.inspect will report, and the two must agree.
            const bool bSynchronizedAfter = Data->IsSynchronizedWithParent();

            TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
            Entry->SetStringField(TEXT("emitter"), HandleName);
            Entry->SetStringField(TEXT("handleId"), Handle.GetId().ToString());
            Entry->SetStringField(TEXT("parentEmitterPath"), Parent->GetPathName());
            Entry->SetBoolField(TEXT("wasStale"), !bSynchronizedBefore);
            Entry->SetBoolField(TEXT("synchronizedWithParent"), bSynchronizedAfter);
            Entry->SetBoolField(TEXT("graphModified"), bGraphModified);
            Entry->SetBoolField(TEXT("merged"), !bAnyFailed);
            if (Errors.Num() > 0)
            {
                Entry->SetArrayField(TEXT("errors"), Errors);
            }
            Refreshed.Add(MakeShared<FJsonValueObject>(Entry));

            if (bAnyFailed)
            {
                ++MergesFailed;
            }
            else
            {
                ++MergesApplied;
            }
        }

        if (InheritedHandles > 0)
        {
            System->MarkPackageDirty();
        }
    }

    bool bCompiled = false;
    PinWrightNiagara::FCompileWaitOutcome CompileWait;
    if (bCompileRequested)
    {
        PinWrightNiagara::KillSystemInstances(*System);
        const bool bCompileIssued = System->RequestCompile(true);
        // See add_emitter: RequestCompile is asynchronous, so the data-interface check and the
        // save below must not read compiled state while it is still in flight.
        CompileWait = PinWrightNiagara::WaitForSystemCompile(*System, bCompileIssued);
        bCompiled = PinWrightNiagara::DidCompileLand(bCompileIssued, CompileWait);
    }

    // Same post-write gate as the two handle verbs: a merge rewrites the emitter's scripts, so it
    // can leave the system's compiled and resolved data-interface sets disagreeing, which is fatal
    // on the next tick.
    PinWrightNiagara::EDataInterfaceConsistency DataInterfaceVerdict =
        PinWrightNiagara::EDataInterfaceConsistency::Unverified;
    {
        TSharedPtr<FJsonObject> MutationFacts = MakeShared<FJsonObject>();
        MutationFacts->SetStringField(TEXT("operation"), TEXT("refresh_emitter"));
        MutationFacts->SetNumberField(TEXT("refreshedEmitters"), Refreshed.Num());
        MutationFacts->SetNumberField(TEXT("mergesApplied"), MergesApplied);
        MutationFacts->SetNumberField(TEXT("mergesFailed"), MergesFailed);
        MutationFacts->SetBoolField(TEXT("compileRequested"), bCompileRequested);
        MutationFacts->SetBoolField(TEXT("compiled"), bCompiled);
        MutationFacts->SetBoolField(TEXT("saveRequested"), bSaveRequested);
        if (!RejectOnDataInterfaceMismatch(Ctx, *System, SystemPath, MutationFacts,
                DataInterfaceVerdictBefore, DataInterfaceVerdict))
        {
            return true;
        }
    }

    bool bSaved = false;
    EAssetSaveState SaveState = EAssetSaveState::NotRequested;
    // See add_emitter: an unlanded compile must not be written to disk. A merge that failed on any
    // handle is not persisted either - that would put a half-reconciled emitter on disk under a
    // response that says the refresh happened.
    const bool bCompileAllowsSave = !bCompileRequested
        || (bCompiled && PinWrightNiagara::MayPersistAfterCompileWait(CompileWait));
    if (bSaveRequested && MergesFailed == 0 && bCompileAllowsSave)
    {
        bSaved = SaveAssetToDiskReportingPresence(
            System, /*bForce=*/true, nullptr, nullptr, &SaveState);
    }
    else if (bSaveRequested)
    {
        SaveState = EAssetSaveState::Failed;
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetBoolField(TEXT("success"), true);
    Result->SetStringField(TEXT("operation"), TEXT("refresh_emitter"));
    Result->SetStringField(TEXT("systemPath"), SystemPath);
    Result->SetNumberField(TEXT("emitterCount"), System->GetEmitterHandles().Num());
    Result->SetNumberField(TEXT("mergesApplied"), MergesApplied);
    Result->SetNumberField(TEXT("mergesFailed"), MergesFailed);
    Result->SetArrayField(TEXT("refreshed"), Refreshed);
    Result->SetArrayField(TEXT("skipped"), Skipped);
    Result->SetBoolField(TEXT("compileRequested"), bCompileRequested);
    Result->SetBoolField(TEXT("compiled"), bCompiled);
    NiagaraEdit::AddNiagaraAssetSaveReport(Result, bSaveRequested, bSaved, SaveState);
    Result->SetStringField(TEXT("dataInterfaceCheck"),
        PinWrightNiagara::DataInterfaceConsistencyToString(DataInterfaceVerdict));
    NiagaraEdit::AddDataInterfaceDelta(Result, /*bBeforeMeasured=*/true,
        DataInterfaceVerdictBefore, DataInterfaceVerdict);
    AddAssetVerification(Result, System);
    Ctx.SendSuccess(Result);
    return true;
}

// ---- niagara.create_system ----
REGISTER_RPC_HANDLER("niagara.create_system", "niagara", "Create an empty UNiagaraSystem asset. Use niagara.add_emitter afterward to populate.",
    RPC_PARAMS(
        RPC_PARAM_REQ("name", "string", "Name of the Niagara system"),
        RPC_PARAM_REQ("savePath", "path", "Content path to save the system")
    ))
{
    FString SystemName;
    if (!Ctx.RequireString(TEXT("name"), SystemName)) return true;

    FString SavePath;
    if (!Ctx.RequireString(TEXT("savePath"), SavePath)) return true;

    if (!FModuleManager::Get().IsModuleLoaded(TEXT("Niagara")))
    {
        Ctx.SendError(TEXT("DEPENDENCY_MISSING"),
            TEXT("Niagara plugin module is not loaded. Please enable and restart the editor."));
        return true;
    }

    FString AssetName = SystemName;

    // COMPOSED AND CHECKED HERE, because the composition is what kills the process. `savePath`
    // and `name` both arrive raw off the wire and used to be concatenated straight into
    // CreatePackage, which logs at Fatal for a name containing "//" (UObjectGlobals.cpp:1094-1096)
    // and for one that resolves to empty (:1118). Fatal is not compiled out in any configuration,
    // so such a call does not fail - it ends the editor PROCESS and every unsaved package in it,
    // and the `if (!Package)` below can never fire because nothing after the call is reached.
    // `name: "/Game/X"` or `name: "a//b"` was a one-argument kill here (board
    // B-createpackage-unvalidated-paths-plugin-wide; the mechanism was measured on
    // B-foliage-add-type-name-with-slash-kills-the-editor). The trailing slash the old
    // composition tolerated is trimmed first, so "/Game/FX/" is still an accepted savePath.
    // The former FPackageName::ObjectPathToPackageName call is gone rather than kept: it only
    // trimmed at a '.' or ':', both of which are now refused outright as object-name characters.
    FString PackagePath = SavePath;
    PackagePath.RemoveFromEnd(TEXT("/"));
    FString ActualPackagePath;
    FString PathError;
    if (!PinWrightComposeAssetPackagePath(PackagePath, AssetName, ActualPackagePath, PathError))
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"),
            FString::Printf(TEXT("%s Pass a bare asset name in 'name' and the destination folder "
                                 "in 'savePath'."), *PathError));
        return true;
    }

    UPackage* Package = CreatePackage(*ActualPackagePath);
    if (!Package)
    {
        Ctx.SendError(TEXT("PACKAGE_ERROR"), TEXT("Failed to create package"));
        return true;
    }

    UNiagaraSystem* NiagaraSystem = NewObject<UNiagaraSystem>(Package, FName(*AssetName), RF_Public | RF_Standalone | RF_Transactional);
    if (NiagaraSystem)
    {
        UNiagaraSystemFactoryNew::InitializeSystem(NiagaraSystem, true);
    }

    if (!NiagaraSystem)
    {
        Ctx.SendError(TEXT("CREATE_FAILED"), TEXT("Failed to create Niagara system"));
        return true;
    }

    FAssetRegistryModule::AssetCreated(NiagaraSystem);

    // Persist to disk for real (not the mark-dirty McpSafeAssetSave) and report
    // saved honestly, gated on the .uasset actually landing on disk.
    const bool bSaved = SaveAssetToDiskReportingPresence(NiagaraSystem, /*bForce=*/true);

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetBoolField(TEXT("success"), true);
    Resp->SetStringField(TEXT("systemPath"), NiagaraSystem->GetPathName());
    Resp->SetStringField(TEXT("systemName"), SystemName);
    Resp->SetBoolField(TEXT("saved"), bSaved);
    if (!bSaved)
    {
        Resp->SetBoolField(TEXT("pendingFlush"), true);
    }
    AddAssetVerification(Resp, NiagaraSystem);

    Ctx.SendSuccess(Resp);
    return true;
}

// ---- niagara.create_emitter ----
REGISTER_RPC_HANDLER("niagara.create_emitter", "niagara", "Create an empty UNiagaraEmitter asset. Standalone emitters can be referenced by multiple systems via niagara.add_emitter.",
    RPC_PARAMS(
        RPC_PARAM_REQ("name", "string", "Name of the Niagara emitter"),
        RPC_PARAM_REQ("savePath", "path", "Content path to save the emitter")
    ))
{
    FString EmitterName;
    if (!Ctx.RequireString(TEXT("name"), EmitterName)) return true;

    FString SavePath;
    if (!Ctx.RequireString(TEXT("savePath"), SavePath)) return true;

    if (!FModuleManager::Get().IsModuleLoaded(TEXT("Niagara")))
    {
        Ctx.SendError(TEXT("DEPENDENCY_MISSING"),
            TEXT("Niagara plugin module is not loaded. Please enable and restart the editor."));
        return true;
    }

    FString AssetName = EmitterName;

    // Same guard, same reason as niagara.create_system above: `savePath` + `name` composed raw
    // into CreatePackage is an editor-process kill on a "//"-bearing or path-shaped `name`
    // (board B-createpackage-unvalidated-paths-plugin-wide). Trailing slash trimmed first so a
    // savePath of "/Game/FX/" keeps working.
    FString PackagePath = SavePath;
    PackagePath.RemoveFromEnd(TEXT("/"));
    FString ActualPackagePath;
    FString PathError;
    if (!PinWrightComposeAssetPackagePath(PackagePath, AssetName, ActualPackagePath, PathError))
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"),
            FString::Printf(TEXT("%s Pass a bare asset name in 'name' and the destination folder "
                                 "in 'savePath'."), *PathError));
        return true;
    }

    UPackage* Package = CreatePackage(*ActualPackagePath);
    if (!Package)
    {
        Ctx.SendError(TEXT("PACKAGE_ERROR"), TEXT("Failed to create package"));
        return true;
    }

    UNiagaraEmitter* NiagaraEmitter = NewObject<UNiagaraEmitter>(Package, FName(*AssetName), RF_Public | RF_Standalone | RF_Transactional);

    if (!NiagaraEmitter)
    {
        Ctx.SendError(TEXT("CREATE_FAILED"), TEXT("Failed to create Niagara emitter"));
        return true;
    }

    UNiagaraEmitterFactoryNew::InitializeEmitter(NiagaraEmitter, false);

    FAssetRegistryModule::AssetCreated(NiagaraEmitter);

    // Persist to disk for real (not the mark-dirty McpSafeAssetSave) and report
    // saved honestly, gated on the .uasset actually landing on disk.
    const bool bSaved = SaveAssetToDiskReportingPresence(NiagaraEmitter, /*bForce=*/true);

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetBoolField(TEXT("success"), true);
    Resp->SetStringField(TEXT("emitterPath"), NiagaraEmitter->GetPathName());
    Resp->SetStringField(TEXT("emitterName"), EmitterName);
    Resp->SetBoolField(TEXT("saved"), bSaved);
    if (!bSaved)
    {
        Resp->SetBoolField(TEXT("pendingFlush"), true);
    }
    AddAssetVerification(Resp, NiagaraEmitter);

    Ctx.SendSuccess(Resp);
    return true;
}

// ---- niagara.spawn_actor ----
REGISTER_RPC_HANDLER("niagara.spawn_actor", "niagara", "Spawn an ANiagaraActor in the active world that plays the given UNiagaraSystem. Convenience for previewing authoring changes without manually placing the actor.",
    RPC_PARAMS(
        RPC_PARAM_REQ("systemPath", "path", "Content path of the Niagara system asset"),
        RPC_PARAM_OPT("location", "object", "Spawn location {x, y, z}"),
        RPC_PARAM_OPT("name", "string", "Label for the spawned actor")
    ))
{
    FString SystemPath;
    if (!Ctx.RequireString(TEXT("systemPath"), SystemPath)) return true;

    double X = 0.0, Y = 0.0, Z = 0.0;
    TSharedPtr<FJsonObject> LocationObj = Ctx.GetObject(TEXT("location"));
    if (LocationObj.IsValid())
    {
        LocationObj->TryGetNumberField(TEXT("x"), X);
        LocationObj->TryGetNumberField(TEXT("y"), Y);
        LocationObj->TryGetNumberField(TEXT("z"), Z);
    }

    FString ActorName = Ctx.GetString(TEXT("name"));

    if (!GEditor || !GEditor->GetEditorWorldContext().World())
    {
        Ctx.SendError(TEXT("EDITOR_NOT_AVAILABLE"), TEXT("Editor world not available"));
        return true;
    }

    UWorld* World = GEditor->GetEditorWorldContext().World();

    if (!ResolveAsset(SystemPath).bExists)
    {
        Ctx.SendError(TEXT("ASSET_NOT_FOUND"),
            FString::Printf(TEXT("Niagara system asset not found: %s"), *SystemPath));
        return true;
    }

    UNiagaraSystem* NiagaraSystem = LoadObject<UNiagaraSystem>(nullptr, *SystemPath);
    if (!NiagaraSystem)
    {
        Ctx.SendError(TEXT("LOAD_FAILED"), TEXT("Failed to load Niagara system"));
        return true;
    }

    FVector Location(X, Y, Z);
    ANiagaraActor* NiagaraActor = World->SpawnActor<ANiagaraActor>(
        ANiagaraActor::StaticClass(), Location, FRotator::ZeroRotator);

    if (!NiagaraActor)
    {
        Ctx.SendError(TEXT("SPAWN_FAILED"), TEXT("Failed to spawn Niagara actor"));
        return true;
    }

    if (NiagaraActor->GetNiagaraComponent())
        NiagaraActor->GetNiagaraComponent()->SetAsset(NiagaraSystem);

    if (!ActorName.IsEmpty())
        NiagaraActor->SetActorLabel(ActorName);
    else
        NiagaraActor->SetActorLabel(
            FString::Printf(TEXT("NiagaraActor_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Short)));

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetBoolField(TEXT("success"), true);
    Resp->SetStringField(TEXT("actorPath"), NiagaraActor->GetPathName());
    Resp->SetStringField(TEXT("actorName"), NiagaraActor->GetActorLabel());
    Resp->SetStringField(TEXT("systemPath"), SystemPath);
    AddActorVerification(Resp, NiagaraActor);

    Ctx.SendSuccess(Resp);
    return true;
}

// ---- niagara.modify_parameter ----
REGISTER_RPC_HANDLER("niagara.modify_parameter", "niagara", "Set a user-exposed parameter on a spawned ANiagaraActor's component (runtime override). For asset-side defaults, inspect the asset first with niagara.inspect or niagara.graph.get, then use niagara.set_parameter.",
    RPC_PARAMS(
        RPC_PARAM_REQ("actorName", "string", "Label of the Niagara actor"),
        RPC_PARAM_REQ("parameterName", "string", "Name of the parameter to modify"),
        RPC_PARAM_OPT("parameterType", "string", "Parameter type: Float, Vector, Color, Bool (default Float)"),
        RPC_PARAM_OPT("type", "string", "Alias for parameterType"),
        RPC_PARAM_REQ("value", "any", "Value to set (number, object, array, or boolean depending on type)")
    ))
{
    FString ActorName;
    if (!Ctx.RequireString(TEXT("actorName"), ActorName)) return true;

    FString ParameterName;
    if (!Ctx.RequireString(TEXT("parameterName"), ParameterName)) return true;

    FString ParameterType = Ctx.GetStringFirstOf({TEXT("parameterType"), TEXT("type")}, TEXT("Float"));

    if (!GEditor || !GEditor->GetEditorWorldContext().World())
    {
        Ctx.SendError(TEXT("EDITOR_NOT_AVAILABLE"), TEXT("Editor world not available"));
        return true;
    }

    UEditorActorSubsystem* ActorSS = GEditor->GetEditorSubsystem<UEditorActorSubsystem>();
    if (!ActorSS)
    {
        Ctx.SendError(TEXT("EDITOR_ACTOR_SUBSYSTEM_MISSING"), TEXT("EditorActorSubsystem not available"));
        return true;
    }

    TArray<AActor*> AllActors = ActorSS->GetAllLevelActors();
    ANiagaraActor* NiagaraActor = nullptr;

    for (AActor* Actor : AllActors)
    {
        if (Actor && Actor->GetActorLabel().Equals(ActorName, ESearchCase::IgnoreCase))
        {
            NiagaraActor = Cast<ANiagaraActor>(Actor);
            if (NiagaraActor) break;
        }
    }

    if (!NiagaraActor || !NiagaraActor->GetNiagaraComponent())
    {
        Ctx.SendError(TEXT("ACTOR_NOT_FOUND"), TEXT("Niagara actor not found"));
        return true;
    }

    UNiagaraComponent* NiagaraComp = NiagaraActor->GetNiagaraComponent();
    bool bSuccess = false;
    // Value read back off the live component's OverrideParameters store after the write, to echo on
    // the result so a set-then-verify loop self-confirms without a fallback object.call_function
    // GetVariable* readback (the only Niagara read RPC, niagara.inspect, reads asset defaults, not
    // this per-actor store). Null until a successful, validated write produces a readback.
    TSharedPtr<FJsonValue> ReadBackValue;
    const TSharedPtr<FJsonObject>& Payload = Ctx.GetRawPayload();

    // Validate the type and reject names not declared on the system in one classification.
    // SetFloatParameter/SetVectorParameter/etc. all forward to
    // OverrideParameters.SetParameterValue(..., bAdd=true), which silently CREATES an entry for
    // any unknown name — so without this check a typo'd / wrong-prefix / wrong-type name writes
    // nothing yet still reported success:true.
    const PinWrightNiagara::EParameterLookup ParamLookup =
        PinWrightNiagara::ClassifyComponentParameter(NiagaraComp, FName(*ParameterName), ParameterType);
    if (ParamLookup == PinWrightNiagara::EParameterLookup::InvalidType)
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), FString::Printf(TEXT("Invalid parameter type: %s"), *ParameterType));
        return true;
    }
    if (ParamLookup == PinWrightNiagara::EParameterLookup::NotFound)
    {
        Ctx.SendError(TEXT("PARAMETER_NOT_FOUND"),
            FString::Printf(TEXT("Niagara system on '%s' has no %s user parameter named '%s'"),
                *ActorName, *ParameterType, *ParameterName));
        return true;
    }

    if (ParameterType.Equals(TEXT("Float"), ESearchCase::IgnoreCase))
    {
        double Value = 0.0;
        if (Payload->TryGetNumberField(TEXT("value"), Value))
        {
            NiagaraComp->SetFloatParameter(FName(*ParameterName), static_cast<float>(Value));
            bSuccess = true;
        }
    }
    else if (ParameterType.Equals(TEXT("Vector"), ESearchCase::IgnoreCase))
    {
        const TSharedPtr<FJsonObject>* VectorObj = nullptr;
        const TArray<TSharedPtr<FJsonValue>>* VectorArr = nullptr;

        if (Payload->TryGetObjectField(TEXT("value"), VectorObj) && VectorObj)
        {
            double VX = 0.0, VY = 0.0, VZ = 0.0;
            (*VectorObj)->TryGetNumberField(TEXT("x"), VX);
            (*VectorObj)->TryGetNumberField(TEXT("y"), VY);
            (*VectorObj)->TryGetNumberField(TEXT("z"), VZ);
            NiagaraComp->SetVectorParameter(FName(*ParameterName), FVector(VX, VY, VZ));
            bSuccess = true;
        }
        else if (Payload->TryGetArrayField(TEXT("value"), VectorArr) && VectorArr && VectorArr->Num() >= 3)
        {
            double VX = (*VectorArr)[0]->AsNumber();
            double VY = (*VectorArr)[1]->AsNumber();
            double VZ = (*VectorArr)[2]->AsNumber();
            NiagaraComp->SetVectorParameter(FName(*ParameterName), FVector(VX, VY, VZ));
            bSuccess = true;
        }
    }
    else if (ParameterType.Equals(TEXT("Color"), ESearchCase::IgnoreCase))
    {
        const TSharedPtr<FJsonObject>* ColorObj = nullptr;
        const TArray<TSharedPtr<FJsonValue>>* ColorArr = nullptr;

        if (Payload->TryGetObjectField(TEXT("value"), ColorObj) && ColorObj)
        {
            double R = 0.0, G = 0.0, B = 0.0, A = 1.0;
            (*ColorObj)->TryGetNumberField(TEXT("r"), R);
            (*ColorObj)->TryGetNumberField(TEXT("g"), G);
            (*ColorObj)->TryGetNumberField(TEXT("b"), B);
            (*ColorObj)->TryGetNumberField(TEXT("a"), A);
            NiagaraComp->SetColorParameter(FName(*ParameterName), FLinearColor(R, G, B, A));
            bSuccess = true;
        }
        else if (Payload->TryGetArrayField(TEXT("value"), ColorArr) && ColorArr && ColorArr->Num() >= 3)
        {
            double R = (*ColorArr)[0]->AsNumber();
            double G = (*ColorArr)[1]->AsNumber();
            double B = (*ColorArr)[2]->AsNumber();
            double A = (ColorArr->Num() > 3) ? (*ColorArr)[3]->AsNumber() : 1.0;
            NiagaraComp->SetColorParameter(FName(*ParameterName), FLinearColor(R, G, B, A));
            bSuccess = true;
        }
    }
    else if (ParameterType.Equals(TEXT("Bool"), ESearchCase::IgnoreCase))
    {
        bool Value = false;
        if (Payload->TryGetBoolField(TEXT("value"), Value))
        {
            NiagaraComp->SetBoolParameter(FName(*ParameterName), Value);
            bSuccess = true;
        }
    }

    // Read the override straight back off the component's OverrideParameters store (the same store
    // the Set*Parameter calls above write to), so the result can echo the value that actually landed.
    // This self-confirms the round-trip from the write result alone: niagara.inspect can't (it reads
    // asset defaults, not this per-actor store). The store is the FNiagaraUserRedirectionParameterStore,
    // whose FindParameterVariable override resolves the "User." prefix the same way the runtime setters
    // do; we then memcpy the raw bytes by the validated type (UNiagaraComponent's GetVariableFloat/Vec3/
    // Color/Bool helpers don't exist before UE 5.7, and the store path is version-agnostic). Vec3 is
    // stored as FVector3f and Bool as a 4-byte FNiagaraBool, so widen on read. The type was already
    // validated by ClassifyComponentParameter above.
    if (bSuccess)
    {
        const FNiagaraParameterStore& OverrideStore = NiagaraComp->GetOverrideParameters();
        auto ReadParam = [&OverrideStore](const FNiagaraTypeDefinition& TypeDef, FName Name, int32 ExpectedSize, const uint8*& OutData) -> bool
        {
            const FNiagaraVariableWithOffset* Found = OverrideStore.FindParameterVariable(FNiagaraVariable(TypeDef, Name));
            if (!Found)
                return false;
            OutData = OverrideStore.GetParameterData(*Found);
            return OutData != nullptr && Found->GetSizeInBytes() == ExpectedSize;
        };

        const FName ParamFName(*ParameterName);
        const uint8* Data = nullptr;
        if (ParameterType.Equals(TEXT("Float"), ESearchCase::IgnoreCase))
        {
            if (ReadParam(FNiagaraTypeDefinition::GetFloatDef(), ParamFName, sizeof(float), Data))
            {
                float Stored = 0.0f;
                FMemory::Memcpy(&Stored, Data, sizeof(float));
                ReadBackValue = MakeShared<FJsonValueNumber>(Stored);
            }
        }
        else if (ParameterType.Equals(TEXT("Vector"), ESearchCase::IgnoreCase))
        {
            // SetVectorParameter writes Vec3; the store keeps it as FVector3f.
            if (ReadParam(FNiagaraTypeDefinition::GetVec3Def(), ParamFName, sizeof(FVector3f), Data))
            {
                FVector3f Stored(0.0f);
                FMemory::Memcpy(&Stored, Data, sizeof(FVector3f));
                ReadBackValue = MakeShared<FJsonValueObject>(JsonBuilders::BuildVectorJson(FVector(Stored)));
            }
        }
        else if (ParameterType.Equals(TEXT("Color"), ESearchCase::IgnoreCase))
        {
            if (ReadParam(FNiagaraTypeDefinition::GetColorDef(), ParamFName, sizeof(FLinearColor), Data))
            {
                FLinearColor Stored;
                FMemory::Memcpy(&Stored, Data, sizeof(FLinearColor));
                ReadBackValue = MakeShared<FJsonValueObject>(JsonBuilders::BuildLinearColorJson(Stored));
            }
        }
        else if (ParameterType.Equals(TEXT("Bool"), ESearchCase::IgnoreCase))
        {
            // FNiagaraBool stores true as INDEX_NONE (-1) / false as 0 in 4 bytes; any nonzero byte means true.
            const FNiagaraVariableWithOffset* Found = OverrideStore.FindParameterVariable(FNiagaraVariable(FNiagaraTypeDefinition::GetBoolDef(), ParamFName));
            if (Found)
            {
                if (const uint8* BoolData = OverrideStore.GetParameterData(*Found))
                {
                    const int32 Size = Found->GetSizeInBytes();
                    bool Stored = false;
                    for (int32 Index = 0; Index < Size; ++Index)
                    {
                        if (BoolData[Index] != 0)
                        {
                            Stored = true;
                            break;
                        }
                    }
                    ReadBackValue = MakeShared<FJsonValueBoolean>(Stored);
                }
            }
        }
    }

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetBoolField(TEXT("success"), bSuccess);
    Resp->SetStringField(TEXT("actorName"), ActorName);
    Resp->SetStringField(TEXT("parameterName"), ParameterName);
    Resp->SetStringField(TEXT("parameterType"), ParameterType);
    if (bSuccess)
    {
        // Echo the override read back off the live component so the set-then-verify loop confirms
        // from this result; overrideStored signals the value was actually present in the store.
        Resp->SetBoolField(TEXT("overrideStored"), ReadBackValue.IsValid());
        if (ReadBackValue.IsValid())
            Resp->SetField(TEXT("value"), ReadBackValue);
    }
    if (bSuccess && NiagaraActor)
        AddActorVerification(Resp, NiagaraActor);

    if (bSuccess)
        Ctx.SendSuccess(Resp);
    else
        Ctx.SendError(TEXT("PARAMETER_SET_FAILED"), TEXT("Failed to modify parameter"));
    return true;
}

// ---- niagara.create_ribbon ----
REGISTER_RPC_HANDLER("niagara.create_ribbon", "niagara", "Spawn a ribbon/beam ANiagaraActor configured with the engine's stock ribbon emitter for quick lasers / trails / contrails.",
    RPC_PARAMS(
        RPC_PARAM_REQ("systemPath", "path", "Content path of the Niagara system to use"),
        RPC_PARAM_OPT("name", "string", "Actor label (default 'NiagaraRibbon')"),
        RPC_PARAM_OPT("start", "object", "Start position {x, y, z}"),
        RPC_PARAM_OPT("end", "object", "End position {x, y, z}"),
        RPC_PARAM_OPT("width", "number", "Ribbon/beam width (default 10)"),
        RPC_PARAM_OPT("color", "object|array", "Color {r, g, b, a} or array [r, g, b, a]")
    ))
{
    FString SystemPath;
    if (!Ctx.RequireString(TEXT("systemPath"), SystemPath)) return true;

    FString Name = Ctx.GetString(TEXT("name"));

    if (!GEditor || !GEditor->GetEditorWorldContext().World())
    {
        Ctx.SendError(TEXT("EDITOR_NOT_AVAILABLE"), TEXT("Editor world not available"));
        return true;
    }

    UWorld* World = GEditor->GetEditorWorldContext().World();

    if (!ResolveAsset(SystemPath).bExists)
    {
        Ctx.SendError(TEXT("ASSET_NOT_FOUND"),
            FString::Printf(TEXT("Niagara system asset not found: %s"), *SystemPath));
        return true;
    }

    UNiagaraSystem* NiagaraSystem = LoadObject<UNiagaraSystem>(nullptr, *SystemPath);
    if (!NiagaraSystem)
    {
        Ctx.SendError(TEXT("LOAD_FAILED"), TEXT("Failed to load Niagara system"));
        return true;
    }

    FVector Start(0, 0, 0);
    TSharedPtr<FJsonObject> StartObj = Ctx.GetObject(TEXT("start"));
    if (StartObj.IsValid())
    {
        double SX = 0, SY = 0, SZ = 0;
        StartObj->TryGetNumberField(TEXT("x"), SX);
        StartObj->TryGetNumberField(TEXT("y"), SY);
        StartObj->TryGetNumberField(TEXT("z"), SZ);
        Start = FVector(SX, SY, SZ);
    }

    ANiagaraActor* NiagaraActor = World->SpawnActor<ANiagaraActor>(
        ANiagaraActor::StaticClass(), Start, FRotator::ZeroRotator);

    if (!NiagaraActor)
    {
        Ctx.SendError(TEXT("SPAWN_FAILED"), TEXT("Failed to spawn Niagara actor"));
        return true;
    }

    NiagaraActor->SetActorLabel(Name.IsEmpty() ? TEXT("NiagaraRibbon") : Name);

    UNiagaraComponent* NiagaraComp = NiagaraActor->GetNiagaraComponent();
    if (NiagaraComp)
    {
        NiagaraComp->SetAsset(NiagaraSystem);
        NiagaraComp->SetVectorParameter(FName("User.RibbonStart"), Start);

        const TSharedPtr<FJsonObject>& Payload = Ctx.GetRawPayload();

        const TSharedPtr<FJsonObject>* EndObj = nullptr;
        if (Payload->TryGetObjectField(TEXT("end"), EndObj) && EndObj)
        {
            double EX = 0, EY = 0, EZ = 0;
            (*EndObj)->TryGetNumberField(TEXT("x"), EX);
            (*EndObj)->TryGetNumberField(TEXT("y"), EY);
            (*EndObj)->TryGetNumberField(TEXT("z"), EZ);
            NiagaraComp->SetVectorParameter(FName("User.RibbonEnd"), FVector(EX, EY, EZ));
            NiagaraComp->SetVectorParameter(FName("User.BeamEnd"), FVector(EX, EY, EZ));
        }

        double Width = 10.0;
        if (Payload->TryGetNumberField(TEXT("width"), Width))
        {
            NiagaraComp->SetFloatParameter(FName("User.RibbonWidth"), (float)Width);
            NiagaraComp->SetFloatParameter(FName("User.BeamWidth"), (float)Width);
        }

        const TSharedPtr<FJsonObject>* ColorObj = nullptr;
        FLinearColor ColorVal(1, 1, 1, 1);
        if (Payload->TryGetObjectField(TEXT("color"), ColorObj) && ColorObj)
        {
            double R = 1, G = 1, B = 1, A = 1;
            (*ColorObj)->TryGetNumberField(TEXT("r"), R);
            (*ColorObj)->TryGetNumberField(TEXT("g"), G);
            (*ColorObj)->TryGetNumberField(TEXT("b"), B);
            (*ColorObj)->TryGetNumberField(TEXT("a"), A);
            ColorVal = FLinearColor(R, G, B, A);
        }
        else
        {
            const TArray<TSharedPtr<FJsonValue>>* ColorArr = nullptr;
            if (Payload->TryGetArrayField(TEXT("color"), ColorArr) && ColorArr && ColorArr->Num() >= 3)
            {
                double R = (*ColorArr)[0]->AsNumber();
                double G = (*ColorArr)[1]->AsNumber();
                double B = (*ColorArr)[2]->AsNumber();
                double A = (ColorArr->Num() > 3) ? (*ColorArr)[3]->AsNumber() : 1.0;
                ColorVal = FLinearColor(R, G, B, A);
            }
        }
        NiagaraComp->SetColorParameter(FName("User.RibbonColor"), ColorVal);
        NiagaraComp->SetColorParameter(FName("User.Color"), ColorVal);
    }

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetBoolField(TEXT("success"), true);
    Resp->SetStringField(TEXT("actorPath"), NiagaraActor->GetPathName());
    Resp->SetStringField(TEXT("actorName"), NiagaraActor->GetActorLabel());
    AddActorVerification(Resp, NiagaraActor);

    Ctx.SendSuccess(Resp);
    return true;
}
