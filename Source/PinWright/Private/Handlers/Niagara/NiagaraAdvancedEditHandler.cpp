// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ParamAliasUtils.h"
#include "Handlers/Niagara/NiagaraEditTypes.h"
#include "Handlers/Niagara/NiagaraJsonHelpers.h"

#include "Handlers/Niagara/NiagaraDataInterfaceConsistency.h"
#include "Handlers/Niagara/NiagaraEditorOpenGuard.h"
#include "Handlers/Niagara/NiagaraSystemViewModelCache.h"
#include "Handlers/Niagara/NiagaraGraphResetUtils.h"
#include "Handlers/Niagara/NiagaraInstanceUtils.h"
#include "Utils/AssetUtils.h"

#include "NiagaraDataInterface.h"
#include "NiagaraEmitter.h"
#include "NiagaraGraph.h"
#include "NiagaraParameterStore.h"
#include "NiagaraScript.h"
#include "NiagaraScriptSource.h"
#include "NiagaraSimulationStageBase.h"
#include "NiagaraSystem.h"
#include "NiagaraTypes.h"
#include "ScopedTransaction.h"
#include "ViewModels/NiagaraEmitterHandleViewModel.h"
#include "ViewModels/NiagaraEmitterViewModel.h"
#include "ViewModels/NiagaraSystemViewModel.h"

namespace
{
    using NiagaraJsonHelpers::SendNiagaraEditError;
    using NiagaraJsonHelpers::BeginEmitterMutationScope;
    using NiagaraJsonHelpers::EndEmitterMutationScope;

    FNiagaraEditError ResolveEmitterTarget(const FString& AssetPath, const FString& EmitterName, FNiagaraResolvedTarget& OutTarget)
    {
        FNiagaraEditTargetSpec Spec;
        Spec.Kind = ENiagaraEditTargetKind::EmitterData;
        Spec.KindText = NiagaraEdit::TargetKindToString(ENiagaraEditTargetKind::EmitterData);
        Spec.EmitterName = EmitterName;
        if (FNiagaraEditError Error = NiagaraEdit::ResolveTarget(AssetPath, Spec, OutTarget); Error.HasError())
        {
            return Error;
        }
        if (!OutTarget.Emitter || !OutTarget.EmitterData)
        {
            return FNiagaraEditError::Make(TEXT("EMITTER_REQUIRED"), TEXT("Resolved Niagara target lacks an emitter."));
        }
        return FNiagaraEditError();
    }

    bool ParseExecutionMode(const FString& Text, EScriptExecutionMode& OutMode, FString& OutError)
    {
        if (Text.IsEmpty())
        {
            return true;
        }
        // Translate user-friendly short aliases to canonical UENUM names so the reflection
        // lookup below stays the single source of truth for the enum values.
        FString Canonical = Text;
        if (Text.Equals(TEXT("Every"), ESearchCase::IgnoreCase))
        {
            Canonical = TEXT("EveryParticle");
        }
        else if (Text.Equals(TEXT("Spawned"), ESearchCase::IgnoreCase))
        {
            Canonical = TEXT("SpawnedParticles");
        }
        else if (Text.Equals(TEXT("Single"), ESearchCase::IgnoreCase))
        {
            Canonical = TEXT("SingleParticle");
        }
        UEnum* EnumPtr = StaticEnum<EScriptExecutionMode>();
        if (!EnumPtr)
        {
            OutError = FString::Printf(TEXT("Could not resolve EScriptExecutionMode reflection for '%s'."), *Text);
            return false;
        }
        // EGetByNameFlags::None defaults to case-insensitive match on the unscoped enum name.
        const int64 Value = EnumPtr->GetValueByNameString(Canonical, EGetByNameFlags::None);
        if (Value == INDEX_NONE)
        {
            OutError = FString::Printf(TEXT("Unknown executionMode '%s'."), *Text);
            return false;
        }
        OutMode = static_cast<EScriptExecutionMode>(Value);
        return true;
    }

    TSharedPtr<FJsonObject> BuildOrphanDataInterfaceJson(const PinWrightNiagara::FOrphanResolvedDataInterface& Orphan)
    {
        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("scriptPath"), Orphan.ScriptPath);
        Entry->SetStringField(TEXT("emitter"), Orphan.EmitterName);
        Entry->SetNumberField(TEXT("resolvedIndex"), Orphan.ResolvedIndex);
        Entry->SetStringField(TEXT("name"), Orphan.Name);
        Entry->SetStringField(TEXT("compileName"), Orphan.CompileName);
        Entry->SetStringField(TEXT("type"), Orphan.TypeName);
        Entry->SetStringField(TEXT("dataInterfaceClass"), Orphan.DataInterfaceClass);
        Entry->SetBoolField(TEXT("internal"), Orphan.bIsInternal);
        return Entry;
    }

    // Same field spelling niagara.add_emitter / remove_emitter put in the
    // NIAGARA_DATA_INTERFACE_MISMATCH payload, so a caller can compare the refusal that sent them
    // here against what these two verbs report without translating between two shapes.
    TSharedPtr<FJsonObject> BuildDataInterfaceMismatchJson(const PinWrightNiagara::FDataInterfaceCountMismatch& Mismatch)
    {
        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("scriptPath"), Mismatch.ScriptPath);
        Entry->SetStringField(TEXT("emitter"), Mismatch.EmitterName);
        Entry->SetNumberField(TEXT("compiledDataInterfaces"), Mismatch.CompiledCount);
        Entry->SetNumberField(TEXT("resolvedDataInterfaces"), Mismatch.ResolvedCount);
        return Entry;
    }
}

REGISTER_RPC_HANDLER("niagara.add_event_handler", "niagara", "Add a particle event handler to a Niagara emitter.",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath", "path", "Path to the Niagara System or Niagara Emitter asset"),
        RPC_PARAM_OPT("emitter", "string", "Emitter name for Niagara System assets"),
        RPC_PARAM_OPT("source", "string", "Source emitter id (Guid string) generating the event"),
        RPC_PARAM_OPT("executionMode", "string", "EveryParticle | SpawnedParticles | SingleParticle"),
        RPC_PARAM_OPT("sourceEventName", "string", "Name of the event the handler reacts to"),
        RPC_PARAM_OPT("spawnNumber", "number", "Number of particles to spawn per event"),
        RPC_PARAM_OPT("maxEventsPerFrame", "integer", "Cap on events consumed per frame"),
        RPC_PARAM_OPT("minSpawnNumber", "number", "Minimum spawn count when bRandomSpawnNumber is true"),
        RPC_PARAM_OPT("bRandomSpawnNumber", "boolean", "Use random spawn count between Min and SpawnNumber"),
        RPC_PARAM_OPT("compile", "boolean", "Compile after edit. Defaults false."),
        RPC_PARAM_OPT("save", "boolean", "Save asset after edit. Defaults false.")
    ))
{
    FNiagaraEventHandlerEditPayload Payload;
    if (SendNiagaraEditError(Ctx, NiagaraEdit::ParseEventHandlerPayload(Ctx.GetRawPayload(), false, Payload))) return true;

    FNiagaraResolvedTarget Target;
    if (SendNiagaraEditError(Ctx, ResolveEmitterTarget(Payload.AssetPath, Payload.EmitterName, Target))) return true;

    if (!Target.System)
    {
        Ctx.SendError(TEXT("SYSTEM_VIEW_MODEL_UNAVAILABLE"), TEXT("Adding an event handler requires a Niagara System asset (the system view model owns the script source)."));
        return true;
    }

    // UNiagaraEmitter::AddEventHandler broadcasts OnEventHandlersChanged, but nothing in the open
    // stack re-reads EventHandlerScriptProps off that: UNiagaraStackEventHandlerPropertiesItem
    // snapshots the whole array into a UNiagaraStackEventWrapper once (only when EmitterObject is
    // null) and writes the snapshot back wholesale from the wrapper's PostEditChangeProperty. An
    // append made here is therefore reverted by the user's next event-handler property edit, with
    // this call having already reported success. No notification can fix that, so refuse instead.
    if (PinWrightNiagara::RejectStructuralEditWhileAssetEditorOpen(
            Ctx, Target.Asset, Target.AssetPath,
            PinWrightNiagara::HazardClause::EventHandlerStackSnapshot))
    {
        return true;
    }

    EScriptExecutionMode ExecutionMode = EScriptExecutionMode::EveryParticle;
    {
        FString ParseError;
        if (!ParseExecutionMode(Payload.ExecutionMode, ExecutionMode, ParseError))
        {
            Ctx.SendError(TEXT("INVALID_ARGUMENT"), ParseError);
            return true;
        }
    }

    FGuid SourceEmitterId;
    if (!Payload.Source.IsEmpty())
    {
        if (!FGuid::Parse(Payload.Source, SourceEmitterId))
        {
            Ctx.SendError(TEXT("INVALID_ARGUMENT"), FString::Printf(TEXT("source '%s' must be a Guid string."), *Payload.Source));
            return true;
        }
    }

    // Resolve view models before opening the transaction so a failure does not leave a
    // partially-applied Modify() inside the undo buffer.
    TSharedRef<FNiagaraSystemViewModel> SVM = PinWrightNiagara::AcquireSystemViewModel(*Target.System);
    const FGuid EmitterHandleId = Target.EmitterHandle ? Target.EmitterHandle->GetId() : FGuid();
    TSharedPtr<FNiagaraEmitterHandleViewModel> EmitterHandleVM = SVM->GetEmitterHandleViewModelById(EmitterHandleId);
    if (!EmitterHandleVM.IsValid())
    {
        Ctx.SendError(TEXT("SYSTEM_VIEW_MODEL_UNAVAILABLE"), TEXT("Could not resolve emitter handle view model from system view model."));
        return true;
    }

    int32 ResultIndex = INDEX_NONE;
    FGuid ResultUsageId;
    {
        FScopedTransaction Transaction(FText::FromString(TEXT("MCP: niagara.add_event_handler")));
        BeginEmitterMutationScope(Target);

        FNiagaraEventScriptProperties Props;
        Props.ExecutionMode = ExecutionMode;
        if (Payload.SpawnNumber != INDEX_NONE)
        {
            Props.SpawnNumber = static_cast<uint32>(FMath::Max(0, Payload.SpawnNumber));
        }
        if (Payload.MaxEventsPerFrame != INDEX_NONE)
        {
            Props.MaxEventsPerFrame = static_cast<uint32>(FMath::Max(0, Payload.MaxEventsPerFrame));
        }
        if (Payload.MinSpawnNumber != INDEX_NONE)
        {
            Props.MinSpawnNumber = static_cast<uint32>(FMath::Max(0, Payload.MinSpawnNumber));
        }
        if (Payload.bHasRandomSpawnNumber)
        {
            Props.bRandomSpawnNumber = Payload.bRandomSpawnNumber;
        }
        if (!Payload.SourceEventName.IsEmpty())
        {
            Props.SourceEventName = FName(*Payload.SourceEventName);
        }
        Props.SourceEmitterID = SourceEmitterId;

        EmitterHandleVM->GetEmitterViewModel()->AddEventHandler(Props, /*bResetGraphForOutput=*/true);

        // Some FNiagaraSystemViewModel sub-viewmodels ignore bIsForDataProcessingOnly and may spin up a preview component during AddEventHandler.
        Target.QuiescedInstances += PinWrightNiagara::KillSystemInstances(*Target.System);

        // AddEventHandler appends without returning the index, so rescan tail of EventHandlerScriptProps.
        Target.EmitterData = Target.Emitter->GetLatestEmitterData();
        if (Target.EmitterData && Target.EmitterData->EventHandlerScriptProps.Num() > 0)
        {
            ResultIndex = Target.EmitterData->EventHandlerScriptProps.Num() - 1;
            const FNiagaraEventScriptProperties& Added = Target.EmitterData->EventHandlerScriptProps[ResultIndex];
            if (Added.Script)
            {
                ResultUsageId = Added.Script->GetUsageId();
            }
        }
    }

    TSharedPtr<FJsonObject> Result = EndEmitterMutationScope(TEXT("add_event_handler"), Target, Payload.Options);
    Result->SetNumberField(TEXT("eventHandlerIndex"), ResultIndex);
    Result->SetStringField(TEXT("eventHandlerId"), ResultUsageId.ToString());
    Ctx.SendSuccess(Result);
    return true;
}

REGISTER_RPC_HANDLER("niagara.remove_event_handler", "niagara", "Remove a Niagara emitter event handler by id or index.",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath", "path", "Path to the Niagara System or Niagara Emitter asset"),
        RPC_PARAM_OPT("emitter", "string", "Emitter name for Niagara System assets"),
        RPC_PARAM_OPT("eventHandlerId", "string", "Event handler usageId Guid"),
        RPC_PARAM_OPT_ALIAS("eventHandlerIndex", "integer", "Event handler index. The bare spelling `index` is accepted for the same slot; eventHandlerIndex wins when both are sent.", "index"),
        RPC_PARAM_OPT("compile", "boolean", "Compile after edit. Defaults false."),
        RPC_PARAM_OPT("save", "boolean", "Save asset after edit. Defaults false.")
    ))
{
    FNiagaraEventHandlerEditPayload Payload;
    if (SendNiagaraEditError(Ctx, NiagaraEdit::ParseEventHandlerPayload(Ctx.GetRawPayload(), true, Payload))) return true;

    FNiagaraResolvedTarget Target;
    if (SendNiagaraEditError(Ctx, ResolveEmitterTarget(Payload.AssetPath, Payload.EmitterName, Target))) return true;

    // Same UNiagaraStackEventWrapper snapshot-and-write-back as add_event_handler, in the direction
    // that loses more: a removal here is undone by the open stack's stale copy.
    if (PinWrightNiagara::RejectStructuralEditWhileAssetEditorOpen(
            Ctx, Target.Asset, Target.AssetPath,
            PinWrightNiagara::HazardClause::EventHandlerStackSnapshot))
    {
        return true;
    }

    FGuid UsageId;
    int32 ResolvedIndex = INDEX_NONE;
    {
        TArray<FNiagaraEventScriptProperties>& Handlers = Target.EmitterData->EventHandlerScriptProps;
        if (!Payload.EntryId.IsEmpty())
        {
            if (!FGuid::Parse(Payload.EntryId, UsageId))
            {
                Ctx.SendError(TEXT("EVENT_HANDLER_INVALID_INDEX"), FString::Printf(TEXT("eventHandlerId '%s' is not a valid Guid."), *Payload.EntryId));
                return true;
            }
            ResolvedIndex = Handlers.IndexOfByPredicate([&UsageId](const FNiagaraEventScriptProperties& Props)
            {
                return Props.Script != nullptr && Props.Script->GetUsageId() == UsageId;
            });
        }
        else
        {
            if (Payload.Index < 0 || Payload.Index >= Handlers.Num())
            {
                Ctx.SendError(TEXT("EVENT_HANDLER_INVALID_INDEX"), FString::Printf(TEXT("eventHandlerIndex %d is out of range."), Payload.Index));
                return true;
            }
            ResolvedIndex = Payload.Index;
            if (Handlers[ResolvedIndex].Script)
            {
                UsageId = Handlers[ResolvedIndex].Script->GetUsageId();
            }
        }
        if (ResolvedIndex == INDEX_NONE || !UsageId.IsValid())
        {
            Ctx.SendError(TEXT("EVENT_HANDLER_NOT_FOUND"), TEXT("Could not resolve event handler usageId."));
            return true;
        }
    }

    {
        FScopedTransaction Transaction(FText::FromString(TEXT("MCP: niagara.remove_event_handler")));
        BeginEmitterMutationScope(Target);

        const FGuid VersionGuid = NiagaraEdit::ResolveEmitterVersionGuid(Target);
        Target.Emitter->RemoveEventHandlerByUsageId(UsageId, VersionGuid);
        Target.EmitterData = Target.Emitter->GetLatestEmitterData();
    }

    TSharedPtr<FJsonObject> Result = EndEmitterMutationScope(TEXT("remove_event_handler"), Target, Payload.Options);
    Result->SetStringField(TEXT("eventHandlerId"), UsageId.ToString());
    Result->SetNumberField(TEXT("eventHandlerIndex"), ResolvedIndex);
    Ctx.SendSuccess(Result);
    return true;
}

REGISTER_RPC_HANDLER("niagara.add_simulation_stage", "niagara", "Add a simulation stage to a Niagara emitter.",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath", "path", "Path to the Niagara System or Niagara Emitter asset"),
        RPC_PARAM_OPT("emitter", "string", "Emitter name for Niagara System assets"),
        RPC_PARAM_OPT("stageClass", "classref", "Simulation stage class path (defaults to UNiagaraSimulationStageGeneric)"),
        RPC_PARAM_OPT("atIndex", "integer", "Optional insertion index"),
        RPC_PARAM_OPT("compile", "boolean", "Compile after edit. Defaults false."),
        RPC_PARAM_OPT("save", "boolean", "Save asset after edit. Defaults false.")
    ))
{
    FNiagaraSimulationStageEditPayload Payload;
    if (SendNiagaraEditError(Ctx, NiagaraEdit::ParseSimulationStagePayload(Ctx.GetRawPayload(), false, Payload))) return true;

    FNiagaraResolvedTarget Target;
    if (SendNiagaraEditError(Ctx, ResolveEmitterTarget(Payload.AssetPath, Payload.EmitterName, Target))) return true;

    UClass* StageClass = nullptr;
    if (Payload.StageClassPath.IsEmpty())
    {
        StageClass = UNiagaraSimulationStageGeneric::StaticClass();
    }
    else
    {
        StageClass = NiagaraEdit::ResolveNiagaraSubclass<UNiagaraSimulationStageBase>(Payload.StageClassPath, TEXT("Niagara"));
        if (!StageClass)
        {
            Ctx.SendError(TEXT("SIMULATION_STAGE_CLASS_NOT_FOUND"), FString::Printf(TEXT("Could not resolve simulation stage class '%s'."), *Payload.StageClassPath));
            return true;
        }
    }
    if (StageClass->HasAnyClassFlags(CLASS_Abstract) || !StageClass->IsChildOf(UNiagaraSimulationStageBase::StaticClass()))
    {
        Ctx.SendError(TEXT("INVALID_SIMULATION_STAGE_CLASS"), FString::Printf(TEXT("Class '%s' is not a concrete UNiagaraSimulationStageBase."), *StageClass->GetPathName()));
        return true;
    }

    // Resolvable preconditions are checked before the FScopedTransaction so a failure here
    // does not leave a half-applied Modify() inside the undo buffer.
    UNiagaraScript* SpawnScript = Target.EmitterData->SpawnScriptProps.Script;
    UNiagaraScriptSource* ScriptSource = SpawnScript ? Cast<UNiagaraScriptSource>(SpawnScript->GetLatestSource()) : nullptr;
    UNiagaraGraph* Graph = ScriptSource ? ScriptSource->NodeGraph : nullptr;
    if (!ScriptSource || !Graph)
    {
        Ctx.SendError(TEXT("GRAPH_NOT_FOUND"), TEXT("Could not resolve emitter spawn script source/graph for simulation stage authoring."));
        return true;
    }

    // Construct the stage + script outside the transaction so a NewObject failure (effectively OOM) does not commit a half-applied Modify() to the undo buffer.
    UNiagaraSimulationStageBase* Stage = NewObject<UNiagaraSimulationStageBase>(Target.Emitter, StageClass, NAME_None, RF_Transactional);
    if (!Stage)
    {
        Ctx.SendError(TEXT("SIMULATION_STAGE_CREATE_FAILED"), FString::Printf(TEXT("Failed to instantiate simulation stage of class '%s'."), *StageClass->GetPathName()));
        return true;
    }
    const FName ScriptName = MakeUniqueObjectName(Stage, UNiagaraScript::StaticClass(), TEXT("SimulationStage"));
    UNiagaraScript* StageScript = NewObject<UNiagaraScript>(Stage, ScriptName, RF_Transactional);
    if (!StageScript)
    {
        Ctx.SendError(TEXT("SIMULATION_STAGE_CREATE_FAILED"), TEXT("Failed to instantiate simulation stage script."));
        return true;
    }

    int32 ResultIndex = INDEX_NONE;
    FGuid ResultUsageId;
    FString ResultStageClass;
    {
        FScopedTransaction Transaction(FText::FromString(TEXT("MCP: niagara.add_simulation_stage")));
        BeginEmitterMutationScope(Target);

        Stage->Modify();
        Stage->Script = StageScript;
        Stage->Script->SetUsage(ENiagaraScriptUsage::ParticleSimulationStageScript);
        // Use Stage->GetMergeId() so plugin call sites that resolve via FVersionedNiagaraEmitterData::GetSimulationStageById find the same Guid that Script->GetUsageId() reports.
        const FGuid NewUsageId = Stage->GetMergeId();
        Stage->Script->SetUsageId(NewUsageId);
        Stage->Script->SetLatestSource(ScriptSource);

        const FGuid VersionGuid = NiagaraEdit::ResolveEmitterVersionGuid(Target);
        Target.Emitter->AddSimulationStage(Stage, VersionGuid);
        if (Payload.AtIndex != INDEX_NONE)
        {
            Target.Emitter->MoveSimulationStageToIndex(Stage, Payload.AtIndex, VersionGuid);
        }

        PinWrightNiagara::ResetGraphForOutput(*Graph, ENiagaraScriptUsage::ParticleSimulationStageScript, NewUsageId);

        Target.EmitterData = Target.Emitter->GetLatestEmitterData();
        ResultIndex = Target.EmitterData ? Target.EmitterData->GetSimulationStages().IndexOfByKey(Stage) : INDEX_NONE;
        ResultUsageId = NewUsageId;
        ResultStageClass = StageClass->GetPathName();
    }

    TSharedPtr<FJsonObject> Result = EndEmitterMutationScope(TEXT("add_simulation_stage"), Target, Payload.Options);
    Result->SetNumberField(TEXT("stageIndex"), ResultIndex);
    Result->SetStringField(TEXT("stageId"), ResultUsageId.ToString());
    Result->SetStringField(TEXT("stageClass"), ResultStageClass);
    Ctx.SendSuccess(Result);
    return true;
}

REGISTER_RPC_HANDLER("niagara.remove_simulation_stage", "niagara", "Remove a Niagara simulation stage by id or index.",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath", "path", "Path to the Niagara System or Niagara Emitter asset"),
        RPC_PARAM_OPT("emitter", "string", "Emitter name for Niagara System assets"),
        RPC_PARAM_OPT("stageId", "string", "Simulation stage usageId Guid"),
        RPC_PARAM_OPT_ALIAS("stageIndex", "integer", "Simulation stage index. The bare spelling `index` is accepted for the same slot; stageIndex wins when both are sent.", "index"),
        RPC_PARAM_OPT("compile", "boolean", "Compile after edit. Defaults false."),
        RPC_PARAM_OPT("save", "boolean", "Save asset after edit. Defaults false.")
    ))
{
    FNiagaraSimulationStageEditPayload Payload;
    if (SendNiagaraEditError(Ctx, NiagaraEdit::ParseSimulationStagePayload(Ctx.GetRawPayload(), true, Payload))) return true;

    FNiagaraResolvedTarget Target;
    if (SendNiagaraEditError(Ctx, ResolveEmitterTarget(Payload.AssetPath, Payload.EmitterName, Target))) return true;

    UNiagaraSimulationStageBase* Stage = nullptr;
    int32 ResolvedIndex = INDEX_NONE;
    FGuid UsageId;
    {
        const TArray<UNiagaraSimulationStageBase*>& Stages = Target.EmitterData->GetSimulationStages();
        if (!Payload.EntryId.IsEmpty())
        {
            if (!FGuid::Parse(Payload.EntryId, UsageId))
            {
                Ctx.SendError(TEXT("SIMULATION_STAGE_INVALID_INDEX"), FString::Printf(TEXT("stageId '%s' is not a valid Guid."), *Payload.EntryId));
                return true;
            }
            Stage = Target.EmitterData->GetSimulationStageById(UsageId);
            ResolvedIndex = Stages.IndexOfByKey(Stage);
        }
        else
        {
            if (Payload.Index < 0 || Payload.Index >= Stages.Num())
            {
                Ctx.SendError(TEXT("SIMULATION_STAGE_INVALID_INDEX"), FString::Printf(TEXT("stageIndex %d is out of range."), Payload.Index));
                return true;
            }
            Stage = Stages[Payload.Index];
            ResolvedIndex = Payload.Index;
            if (Stage && Stage->Script)
            {
                UsageId = Stage->Script->GetUsageId();
            }
        }
        if (!Stage)
        {
            Ctx.SendError(TEXT("SIMULATION_STAGE_NOT_FOUND"), TEXT("Could not resolve simulation stage."));
            return true;
        }
    }

    {
        FScopedTransaction Transaction(FText::FromString(TEXT("MCP: niagara.remove_simulation_stage")));
        BeginEmitterMutationScope(Target);

        const FGuid VersionGuid = NiagaraEdit::ResolveEmitterVersionGuid(Target);
        Target.Emitter->RemoveSimulationStage(Stage, VersionGuid);
        Target.EmitterData = Target.Emitter->GetLatestEmitterData();
    }

    TSharedPtr<FJsonObject> Result = EndEmitterMutationScope(TEXT("remove_simulation_stage"), Target, Payload.Options);
    Result->SetStringField(TEXT("stageId"), UsageId.ToString());
    Result->SetNumberField(TEXT("stageIndex"), ResolvedIndex);
    Ctx.SendSuccess(Result);
    return true;
}

REGISTER_RPC_HANDLER("niagara.add_data_interface", "niagara", "Add a data interface entry to a Niagara parameter store.",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath", "path", "Path to the Niagara System or Niagara Emitter asset"),
        RPC_PARAM_REQ("scope", "string", "Parameter scope (user, rendererBindings, etc.)"),
        RPC_PARAM_REQ_ALIAS("parameterName", "string", "Parameter name for the new data interface. The shorter spelling `name` is accepted for the same slot.", "name"),
        RPC_PARAM_REQ("dataInterfaceClass", "classref", "UNiagaraDataInterface subclass path"),
        RPC_PARAM_OPT("emitter", "string", "Emitter name for the emitter-scoped stores (rendererBindings, spawnRapidIteration, updateRapidIteration). The system-wide scopes (user, systemSpawnRapidIteration, systemUpdateRapidIteration) reject it with INVALID_ARGUMENT because their stores live on the system"),
        RPC_PARAM_OPT("compile", "boolean", "Compile after edit. Defaults false."),
        RPC_PARAM_OPT("save", "boolean", "Save asset after edit. Defaults false.")
    ))
{
    FNiagaraDataInterfaceEditPayload Payload;
    if (SendNiagaraEditError(Ctx, NiagaraEdit::ParseDataInterfacePayload(Ctx.GetRawPayload(), true, Payload))) return true;

    FNiagaraEditTargetSpec Spec;
    Spec.Kind = ENiagaraEditTargetKind::ParameterStore;
    Spec.KindText = NiagaraEdit::TargetKindToString(ENiagaraEditTargetKind::ParameterStore);
    Spec.Scope = Payload.Scope;
    Spec.EmitterName = Payload.EmitterName;

    FNiagaraResolvedTarget Target;
    if (SendNiagaraEditError(Ctx, NiagaraEdit::ResolveTarget(Payload.AssetPath, Spec, Target))) return true;
    if (!Target.ParameterStore)
    {
        Ctx.SendError(TEXT("PARAMETER_STORE_NOT_FOUND"), FString::Printf(TEXT("Parameter store for scope '%s' is not available."), *Payload.Scope));
        return true;
    }

    UClass* DIClass = NiagaraEdit::ResolveNiagaraSubclass<UNiagaraDataInterface>(Payload.DataInterfaceClassPath, TEXT("Niagara"));
    if (!DIClass)
    {
        Ctx.SendError(TEXT("DATA_INTERFACE_CLASS_NOT_FOUND"), FString::Printf(TEXT("Could not resolve data interface class '%s'."), *Payload.DataInterfaceClassPath));
        return true;
    }
    if (DIClass->HasAnyClassFlags(CLASS_Abstract) || !DIClass->IsChildOf(UNiagaraDataInterface::StaticClass()))
    {
        Ctx.SendError(TEXT("INVALID_DATA_INTERFACE_CLASS"), FString::Printf(TEXT("Class '%s' is not a concrete UNiagaraDataInterface."), *DIClass->GetPathName()));
        return true;
    }

    UObject* OuterAsset = Target.System ? static_cast<UObject*>(Target.System) : static_cast<UObject*>(Target.Emitter);
    if (!OuterAsset)
    {
        Ctx.SendError(TEXT("PARAMETER_STORE_NOT_FOUND"), TEXT("No outer asset available for data interface allocation."));
        return true;
    }

    FNiagaraVariable Variable(FNiagaraTypeDefinition(DIClass), FName(*Payload.ParameterName));
    if (Target.ParameterStore->FindParameterVariable(Variable) != nullptr)
    {
        Ctx.SendError(TEXT("DATA_INTERFACE_EXISTS"), FString::Printf(TEXT("Parameter '%s' already exists in scope '%s'."), *Payload.ParameterName, *Payload.Scope));
        return true;
    }

    // Construct the data interface outside the transaction so a NewObject failure (effectively OOM) does not commit a half-applied AddParameter to the undo buffer.
    const FName DIName = MakeUniqueObjectName(OuterAsset, DIClass, FName(*FString::Printf(TEXT("%s_%s"), *DIClass->GetName(), *Payload.ParameterName)));
    UNiagaraDataInterface* NewDI = NewObject<UNiagaraDataInterface>(OuterAsset, DIClass, DIName, RF_Transactional);
    if (!NewDI)
    {
        Ctx.SendError(TEXT("INVALID_DATA_INTERFACE_CLASS"), FString::Printf(TEXT("Failed to instantiate data interface '%s'."), *DIClass->GetPathName()));
        return true;
    }

    {
        FScopedTransaction Transaction(FText::FromString(TEXT("MCP: niagara.add_data_interface")));
        OuterAsset->Modify();
        if (Target.Asset && Target.Asset != OuterAsset)
        {
            Target.Asset->Modify();
        }

        if (!Target.ParameterStore->AddParameter(Variable, /*bInitialize=*/true, /*bTriggerRebind=*/true))
        {
            Transaction.Cancel();
            Ctx.SendError(TEXT("DATA_INTERFACE_EXISTS"), FString::Printf(TEXT("Failed to add parameter '%s' to scope '%s'."), *Payload.ParameterName, *Payload.Scope));
            return true;
        }
        Target.ParameterStore->SetDataInterface(NewDI, Variable);
    }

    TSharedPtr<FJsonObject> Result = EndEmitterMutationScope(TEXT("add_data_interface"), Target, Payload.Options);
    Result->SetStringField(TEXT("scope"), Payload.Scope);
    Result->SetStringField(TEXT("parameterName"), Payload.ParameterName);
    Result->SetStringField(TEXT("dataInterfaceClass"), DIClass->GetPathName());
    Ctx.SendSuccess(Result);
    return true;
}

REGISTER_RPC_HANDLER("niagara.remove_data_interface", "niagara", "Remove a Niagara data interface entry by parameter name.",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath", "path", "Path to the Niagara System or Niagara Emitter asset"),
        RPC_PARAM_REQ("scope", "string", "Parameter scope"),
        RPC_PARAM_REQ_ALIAS("parameterName", "string", "Parameter name to remove. The shorter spelling `name` is accepted for the same slot.", "name"),
        RPC_PARAM_OPT("emitter", "string", "Emitter name for the emitter-scoped stores (rendererBindings, spawnRapidIteration, updateRapidIteration). The system-wide scopes (user, systemSpawnRapidIteration, systemUpdateRapidIteration) reject it with INVALID_ARGUMENT because their stores live on the system"),
        RPC_PARAM_OPT("compile", "boolean", "Compile after edit. Defaults false."),
        RPC_PARAM_OPT("save", "boolean", "Save asset after edit. Defaults false.")
    ))
{
    FNiagaraDataInterfaceEditPayload Payload;
    if (SendNiagaraEditError(Ctx, NiagaraEdit::ParseDataInterfacePayload(Ctx.GetRawPayload(), false, Payload))) return true;

    FNiagaraEditTargetSpec Spec;
    Spec.Kind = ENiagaraEditTargetKind::ParameterStore;
    Spec.KindText = NiagaraEdit::TargetKindToString(ENiagaraEditTargetKind::ParameterStore);
    Spec.Scope = Payload.Scope;
    Spec.EmitterName = Payload.EmitterName;

    FNiagaraResolvedTarget Target;
    if (SendNiagaraEditError(Ctx, NiagaraEdit::ResolveTarget(Payload.AssetPath, Spec, Target))) return true;
    if (!Target.ParameterStore)
    {
        Ctx.SendError(TEXT("PARAMETER_STORE_NOT_FOUND"), FString::Printf(TEXT("Parameter store for scope '%s' is not available."), *Payload.Scope));
        return true;
    }

    // remove_data_interface only has the parameter name (not a type), so a name-based scan is required to recover the full FNiagaraVariable RemoveParameter needs.
    // Two-pass match: first look up by exact name (e.g. emitter-scoped stores), then by user-namespace prefix so the User redirection store ("User.<name>") also resolves.
    const FName TargetName(*Payload.ParameterName);
    const FName UserNamespacedName(*FString::Printf(TEXT("User.%s"), *Payload.ParameterName));
    FNiagaraVariable Found;
    bool bFoundParameter = false;
    TArray<FNiagaraVariable> Existing;
    Target.ParameterStore->GetParameters(Existing);
    for (const FNiagaraVariable& Var : Existing)
    {
        if (Var.GetName() == TargetName || Var.GetName() == UserNamespacedName)
        {
            Found = Var;
            bFoundParameter = true;
            break;
        }
    }
    if (!bFoundParameter)
    {
        Ctx.SendError(TEXT("DATA_INTERFACE_NOT_FOUND"), FString::Printf(TEXT("Parameter '%s' was not found in scope '%s'."), *Payload.ParameterName, *Payload.Scope));
        return true;
    }

    {
        FScopedTransaction Transaction(FText::FromString(TEXT("MCP: niagara.remove_data_interface")));
        if (Target.Asset)
        {
            Target.Asset->Modify();
        }
        if (!Target.ParameterStore->RemoveParameter(Found))
        {
            Transaction.Cancel();
            Ctx.SendError(TEXT("DATA_INTERFACE_NOT_FOUND"), FString::Printf(TEXT("Failed to remove parameter '%s'."), *Payload.ParameterName));
            return true;
        }
    }

    TSharedPtr<FJsonObject> Result = EndEmitterMutationScope(TEXT("remove_data_interface"), Target, Payload.Options);
    Result->SetStringField(TEXT("scope"), Payload.Scope);
    Result->SetStringField(TEXT("parameterName"), Payload.ParameterName);
    Ctx.SendSuccess(Result);
    return true;
}

// ------------------------------------------------------------------------------------------------
// Orphan RESOLVED data interfaces.
//
// A different set from the two verbs above. add_data_interface / remove_data_interface address
// PARAMETER STORE entries - authored, named, visible in the Niagara editor's parameter panel. The
// two verbs below address a script's RESOLVED set (FNiagaraScriptRuntimeCompiledData::
// ResolvedDataInterfaces) and the compile-time cached defaults it is rebuilt from: derived
// plumbing that nothing in the editor UI shows, and the set NIAGARA_DATA_INTERFACE_MISMATCH counts.
// Removing a parameter-store entry cannot clear that refusal, and these two cannot delete an
// authored parameter.
// ------------------------------------------------------------------------------------------------

REGISTER_RPC_HANDLER("niagara.list_orphan_data_interfaces", "niagara",
    "List the resolved data interfaces a Niagara system's compiled scripts no longer reference.",
    RPC_PARAMS(
        RPC_PARAM_REQ("systemPath", "path", "Asset path of the Niagara system")
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

    TArray<PinWrightNiagara::FOrphanResolvedDataInterface> Orphans;
    const PinWrightNiagara::EDataInterfaceConsistency Verdict =
        PinWrightNiagara::FindOrphanResolvedDataInterfaces(*System, Orphans);

    // Reported beside the orphans rather than instead of them: the counts say the system is unsafe
    // to tick, the orphan list says what a removal would act on, and neither implies the other.
    TArray<PinWrightNiagara::FDataInterfaceCountMismatch> Mismatches;
    PinWrightNiagara::CheckDataInterfaceCounts(*System, Mismatches);

    TArray<TSharedPtr<FJsonValue>> OrphanValues;
    OrphanValues.Reserve(Orphans.Num());
    for (const PinWrightNiagara::FOrphanResolvedDataInterface& Orphan : Orphans)
    {
        OrphanValues.Add(MakeShared<FJsonValueObject>(BuildOrphanDataInterfaceJson(Orphan)));
    }

    TArray<TSharedPtr<FJsonValue>> MismatchValues;
    MismatchValues.Reserve(Mismatches.Num());
    for (const PinWrightNiagara::FDataInterfaceCountMismatch& Mismatch : Mismatches)
    {
        MismatchValues.Add(MakeShared<FJsonValueObject>(BuildDataInterfaceMismatchJson(Mismatch)));
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetBoolField(TEXT("success"), true);
    Result->SetStringField(TEXT("operation"), TEXT("list_orphan_data_interfaces"));
    Result->SetStringField(TEXT("systemPath"), SystemPath);
    // "unverified" means nothing could be compared, so an empty orphans[] is not evidence there
    // are none. Compile the system and ask again.
    Result->SetStringField(TEXT("dataInterfaceCheck"),
        PinWrightNiagara::DataInterfaceConsistencyToString(Verdict));
    Result->SetNumberField(TEXT("orphanCount"), Orphans.Num());
    Result->SetArrayField(TEXT("orphans"), OrphanValues);
    Result->SetArrayField(TEXT("mismatchedScripts"), MismatchValues);
    Ctx.SendSuccess(Result);
    return true;
}

REGISTER_RPC_HANDLER("niagara.remove_orphan_data_interfaces", "niagara",
    "Remove the resolved data interfaces a Niagara system's compiled scripts no longer reference.",
    RPC_PARAMS(
        RPC_PARAM_REQ("systemPath", "path", "Asset path of the Niagara system"),
        RPC_PARAM_OPT("save", "boolean", "Save the system asset when something was removed. Defaults false.")
    ))
{
    FString SystemPath;
    if (!Ctx.RequireString(TEXT("systemPath"), SystemPath)) return true;
    const bool bSaveRequested = Ctx.GetBool(TEXT("save"), false);

    UNiagaraSystem* System = LoadObject<UNiagaraSystem>(nullptr, *SystemPath);
    if (!System)
    {
        Ctx.SendError(TEXT("ASSET_NOT_FOUND"), FString::Printf(TEXT("Could not load Niagara system '%s'."), *SystemPath));
        return true;
    }

    // Quiesce only when there is something to remove. The verb has to be idempotent, so a repeat
    // of a landed repair is a normal call shape - and stopping every preview of the system to then
    // change nothing is a side effect the caller cannot see coming from a no-op response.
    //
    // No EDITOR_OPEN guard, deliberately - see the adoption rule in NiagaraEditorOpenGuard.h. The
    // resolved set is derived plumbing no Niagara stack widget snapshots or holds a bare pointer
    // into; what does read it is a live FNiagaraSystemInstance, on a worker thread, which is what
    // the quiesce is for. UNiagaraSystem::OnCompiledDataInterfaceChanged (run inside the repair)
    // is the engine's own notification for this change, so an open toolkit is told.
    {
        TArray<PinWrightNiagara::FOrphanResolvedDataInterface> Planned;
        PinWrightNiagara::FindOrphanResolvedDataInterfaces(*System, Planned);
        if (Planned.Num() > 0)
        {
            PinWrightNiagara::KillSystemInstances(*System);
        }
    }

    PinWrightNiagara::FOrphanRemovalReport Report;
    PinWrightNiagara::EOrphanRemovalOutcome Outcome = PinWrightNiagara::EOrphanRemovalOutcome::Unverified;
    {
        FScopedTransaction Transaction(FText::FromString(TEXT("MCP: niagara.remove_orphan_data_interfaces")));
        Outcome = PinWrightNiagara::RemoveOrphanResolvedDataInterfaces(*System, Report);
        if (Outcome != PinWrightNiagara::EOrphanRemovalOutcome::Removed)
        {
            Transaction.Cancel();
        }
    }

    if (Outcome == PinWrightNiagara::EOrphanRemovalOutcome::RefusedIrreconcilable)
    {
        TArray<TSharedPtr<FJsonValue>> BlockedValues;
        BlockedValues.Reserve(Report.Irreconcilable.Num());
        for (const PinWrightNiagara::FDataInterfaceCountMismatch& Blocked : Report.Irreconcilable)
        {
            BlockedValues.Add(MakeShared<FJsonValueObject>(BuildDataInterfaceMismatchJson(Blocked)));
        }

        TSharedPtr<FJsonObject> ErrorData = MakeShared<FJsonObject>();
        ErrorData->SetStringField(TEXT("systemPath"), SystemPath);
        ErrorData->SetStringField(TEXT("outcome"), PinWrightNiagara::OrphanRemovalOutcomeToString(Outcome));
        ErrorData->SetNumberField(TEXT("removedCount"), 0);
        ErrorData->SetArrayField(TEXT("blockedScripts"), BlockedValues);
        ErrorData->SetBoolField(TEXT("saved"), false);

        Ctx.SendError(TEXT("NIAGARA_ORPHAN_REMOVAL_UNSAFE"), FString::Printf(
            TEXT("Dropping the orphan resolved data interfaces of '%s' would not bring these scripts' ")
            TEXT("resolved count back to their compiled count: %s. Either the compiled list is the ")
            TEXT("longer one, so no removal can close the gap, or the two lists agree in count while ")
            TEXT("disagreeing by name - in which case a leftover is standing in for an entry the ")
            TEXT("bytecode still wants, and removing it would arm the VectorVM assert rather than ")
            TEXT("disarm it. Nothing was changed. Recompile the system (niagara.compile {force: true, ")
            TEXT("wait: true}) instead."),
            *SystemPath,
            *PinWrightNiagara::DescribeDataInterfaceMismatches(Report.Irreconcilable)), ErrorData);
        return true;
    }

    const bool bRemoved = Outcome == PinWrightNiagara::EOrphanRemovalOutcome::Removed;
    if (bRemoved)
    {
        System->MarkPackageDirty();
    }

    // Saving is scoped to a call that actually changed something: a repeat of a landed repair must
    // converge rather than rewrite an unchanged package, so `saveRequested: true, saved: false`
    // beside `outcome: "nothing_to_remove"` is the honest report, not a failure.
    bool bSaved = false;
    if (bSaveRequested && bRemoved)
    {
        bSaved = SaveAssetToDiskReportingPresence(System, /*bForce=*/true);
    }

    TArray<TSharedPtr<FJsonValue>> RemovedValues;
    RemovedValues.Reserve(Report.Removed.Num());
    for (const PinWrightNiagara::FOrphanResolvedDataInterface& Orphan : Report.Removed)
    {
        RemovedValues.Add(MakeShared<FJsonValueObject>(BuildOrphanDataInterfaceJson(Orphan)));
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetBoolField(TEXT("success"), true);
    Result->SetStringField(TEXT("operation"), TEXT("remove_orphan_data_interfaces"));
    Result->SetStringField(TEXT("systemPath"), SystemPath);
    Result->SetStringField(TEXT("outcome"), PinWrightNiagara::OrphanRemovalOutcomeToString(Outcome));
    Result->SetNumberField(TEXT("removedCount"), Report.Removed.Num());
    Result->SetArrayField(TEXT("removed"), RemovedValues);
    Result->SetNumberField(TEXT("scriptsTouched"), Report.ScriptsTouched);
    // Scripts whose serialized cached-default list was pruned too. A script counted in
    // scriptsTouched but not here keeps its orphans in the list the next resolve rebuilds from, so
    // they come back on the system's next compile.
    Result->SetNumberField(TEXT("scriptsPrunedDurably"), Report.ScriptsPrunedDurably);
    // Re-measured after the removal, not inferred from it. This is the field that says whether the
    // system is safe to tick now.
    Result->SetStringField(TEXT("dataInterfaceCheck"),
        PinWrightNiagara::DataInterfaceConsistencyToString(Report.VerdictAfter));
    Result->SetBoolField(TEXT("saveRequested"), bSaveRequested);
    Result->SetBoolField(TEXT("saved"), bSaved);
    AddAssetVerification(Result, System);
    Ctx.SendSuccess(Result);
    return true;
}
