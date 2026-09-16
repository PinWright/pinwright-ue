// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Handlers/Niagara/NiagaraDataInterfaceConsistency.h"
#include "Utils/AssetSaveState.h"
#include "UObject/UnrealType.h"

class UEdGraphNode;
class UEdGraphPin;
class UEnum;
class UNiagaraDataInterface;
class UNiagaraEmitter;
class UNiagaraGraph;
class UNiagaraNodeFunctionCall;
class UNiagaraNodeStaticSwitch;
class UNiagaraRendererProperties;
class UNiagaraScriptVariable;
class UNiagaraScript;
class UNiagaraSystem;
enum class ENiagaraStaticSwitchType : uint8;
enum class ENiagaraScriptUsage : uint8;
struct FNiagaraEmitterHandle;
struct FNiagaraParameterStore;
struct FNiagaraVariable;
struct FVersionedNiagaraEmitterData;

enum class ENiagaraEditTargetKind : uint8
{
    Unknown,
    System,
    EmitterHandle,
    EmitterData,
    Renderer,
    ParameterStore,
    Graph,
    Node,
    Pin,
    DataInterface,
    Module,
    EventHandler,
    SimulationStage
};

enum class ENiagaraEditOperation : uint8
{
    SetProperty,
    SetParameter,
    AddParameter,
    RemoveParameter,
    AddModule,
    RemoveModule,
    MoveModule,
    SetStackEnabled,
    SetModuleInput,
    SetModuleScript,
    ResetModuleInput,
    ClearModuleOverrides,
    AddRenderer,
    RemoveRenderer,
    MoveRenderer,
    ConnectPin,
    DisconnectPin,
    SetPinDefault
};

struct FNiagaraEditError
{
    FString Code;
    FString Message;

    bool HasError() const { return !Code.IsEmpty(); }
    static FNiagaraEditError Make(const TCHAR* InCode, const FString& InMessage);
};

struct FNiagaraEditOptions
{
    bool bCompile = false;
    bool bSave = false;
};

struct FNiagaraEditTargetSpec
{
    ENiagaraEditTargetKind Kind = ENiagaraEditTargetKind::Unknown;
    FString KindText;
    FString EmitterName;
    FString Scope;
    FString ScriptUsage;
    FString NodeId;
    FString PinName;
    FString EntryId;
    // Owner half of an owner-qualified `entryId` ("<ownerName>:<nodeGuid>"), empty when the
    // caller passed a bare id. ParseTargetSpec splits it off; ResolveTarget uses it to select
    // the emitter when none was named, and to refuse an id whose owner disagrees with the
    // emitter that was (B-niagara-entry-id-not-unique-across-emitters).
    FString EntryOwner;
    int32 Index = INDEX_NONE;
    int32 ToIndex = INDEX_NONE;
};

struct FNiagaraResolvedTarget
{
    UObject* Asset = nullptr;
    UNiagaraSystem* System = nullptr;
    UNiagaraEmitter* Emitter = nullptr;
    FNiagaraEmitterHandle* EmitterHandle = nullptr;
    FVersionedNiagaraEmitterData* EmitterData = nullptr;
    FNiagaraParameterStore* ParameterStore = nullptr;
    UNiagaraRendererProperties* Renderer = nullptr;
    UNiagaraGraph* Graph = nullptr;
    UEdGraphNode* Node = nullptr;
    UEdGraphPin* Pin = nullptr;
    UNiagaraDataInterface* DataInterface = nullptr;
    UNiagaraNodeFunctionCall* ModuleNode = nullptr;
    UObject* ReflectedObject = nullptr;
    UStruct* ReflectedStruct = nullptr;
    void* ReflectedContainer = nullptr;
    int32 EventHandlerIndex = INDEX_NONE;
    int32 SimulationStageIndex = INDEX_NONE;
    // Running system instances this call has stopped so far. Not part of the resolved identity:
    // it accumulates as the mutation runs, from BeginEmitterMutationScope and from
    // RequestNiagaraCompile, and MakeMutationResult reports it as `quiescedInstances`. Carrying it
    // on the target is what lets every mutation verb disclose the blanked preview without an extra
    // out-parameter threaded through 20+ call sites
    // (B-niagara-mutation-scope-blanks-open-preview, E-niagara-mutation-result-no-quiesced-count).
    int32 QuiescedInstances = 0;
    // Compiled-vs-resolved data-interface verdict for this call, measured by FinalizeNiagaraEdit
    // after the compile wait and before the save, and reported by MakeMutationResult as
    // `dataInterfaceCheck` / `mismatchedScripts`. Carried here for the same reason
    // QuiescedInstances is: it is produced during the mutation and needed by the envelope, and
    // the alternative is an out-parameter threaded through FinalizeNiagaraEdit's 24 call sites.
    //
    // Stays Unverified when the edit never reached FinalizeNiagaraEdit, and when the edited asset
    // is a standalone Niagara Emitter — ResolveTarget sets Target.System only for UNiagaraSystem
    // assets, and the check needs a system to compare. Unverified is not a pass
    // (B-niagara-finalize-edit-no-di-gate).
    PinWrightNiagara::EDataInterfaceConsistency DataInterfaceVerdict =
        PinWrightNiagara::EDataInterfaceConsistency::Unverified;
    // Only filled for Mismatched.
    TArray<PinWrightNiagara::FDataInterfaceCountMismatch> DataInterfaceMismatches;
    // The same verdict measured BEFORE the mutation ran, by RecordDataInterfaceVerdictBefore from
    // whichever "about to mutate" seam the verb went through. Without it a response can only echo
    // the state it is in, which is the defect: an emitter-scoped write invalidates that emitter's
    // compiled scripts unconditionally, so `mismatched` came back on every such call with nothing
    // saying whether the caller inherited it or created it
    // (B-niagara-set-parameter-emitter-scope-arms-di-mismatch).
    PinWrightNiagara::EDataInterfaceConsistency DataInterfaceVerdictBefore =
        PinWrightNiagara::EDataInterfaceConsistency::Unverified;
    // Distinguishes "measured, and it was Unverified" from "never measured", which the enum
    // alone cannot. Only a measured before-verdict may be published as one.
    bool bDataInterfaceVerdictBeforeMeasured = false;
    // What FinalizeNiagaraEdit did about a mismatch this write armed on a system something was
    // ticking. NotNeeded covers both "nothing was armed" and "nothing was live to detonate it".
    enum class EDataInterfaceRepair : uint8
    {
        NotNeeded,
        // Live instances were quiesced and a compile was awaited; the re-measured verdict is no
        // longer Mismatched.
        Recompiled,
        // The same attempt ran and the system is still Mismatched. The instances are gone, so
        // nothing ticks it, but the asset is not sound and the save is refused.
        Failed
    };
    EDataInterfaceRepair DataInterfaceRepair = EDataInterfaceRepair::NotNeeded;
    // Live instances counted before the repair quiesced them. 0 whenever no repair ran.
    int32 DataInterfaceLiveInstances = 0;
    // Exactly the systems whose RequestCompile calls returned true for this target. Emitter
    // compiles need this because the engine's RequestCompileForEmitter helper returns void and
    // discards both the affected systems and the per-system request result.
    TArray<TWeakObjectPtr<UNiagaraSystem>> CompileRequestSystems;
    // Persistence outcome from FinalizeNiagaraEdit, carried to MakeMutationResult so the wire
    // response keeps the shared AssetSaveState shape instead of collapsing every refusal to
    // saved:false.
    EAssetSaveState SaveState = EAssetSaveState::NotRequested;
    FString AssetPath;
    FString AssetKind;
};

struct FNiagaraPropertyEditPayload
{
    FString AssetPath;
    FNiagaraEditTargetSpec Target;
    FString PropertyPath;
    TSharedPtr<FJsonValue> Value;
    FNiagaraEditOptions Options;
};

struct FNiagaraParameterEditPayload
{
    FString AssetPath;
    FString Scope;
    FString EmitterName;
    FString Name;
    FString Type;
    TSharedPtr<FJsonValue> Value;
    FNiagaraEditOptions Options;
};

struct FNiagaraRendererEditPayload
{
    FString AssetPath;
    FNiagaraEditTargetSpec Target;
    FString RendererClassPath;
    FNiagaraEditOptions Options;
};

struct FNiagaraModuleEditPayload
{
    FString AssetPath;
    FNiagaraEditTargetSpec Target;
    FString ModulePath;
    FString InputName;
    TSharedPtr<FJsonValue> Value;
    FNiagaraEditOptions Options;
    // SetModuleScript fields
    FString NewScriptPath;
    FGuid NewScriptVersion;
    bool bPreserveOverrides = true;
};

struct FNiagaraPinEditPayload
{
    FString AssetPath;
    FNiagaraEditTargetSpec Target;
    FString FromNode;
    FString FromPin;
    FString ToNode;
    FString ToPin;
    FString NodeId;
    FString PinName;
    TSharedPtr<FJsonValue> DefaultValue;
    FNiagaraEditOptions Options;
};

struct FNiagaraEventHandlerEditPayload
{
    FString AssetPath;
    FString EmitterName;
    FString EntryId;
    int32 Index = INDEX_NONE;
    FString Source;
    FString ExecutionMode;
    FString SourceEventName;
    int32 SpawnNumber = INDEX_NONE;
    int32 MaxEventsPerFrame = INDEX_NONE;
    int32 MinSpawnNumber = INDEX_NONE;
    bool bRandomSpawnNumber = false;
    bool bHasRandomSpawnNumber = false;
    FNiagaraEditOptions Options;
};

struct FNiagaraSimulationStageEditPayload
{
    FString AssetPath;
    FString EmitterName;
    FString EntryId;
    int32 Index = INDEX_NONE;
    FString StageClassPath;
    int32 AtIndex = INDEX_NONE;
    FNiagaraEditOptions Options;
};

struct FNiagaraDataInterfaceEditPayload
{
    FString AssetPath;
    FString Scope;
    FString EmitterName;
    FString ParameterName;
    FString DataInterfaceClassPath;
    FNiagaraEditOptions Options;
};

namespace NiagaraEdit
{
    PINWRIGHT_API FString TargetKindToString(ENiagaraEditTargetKind Kind);
    PINWRIGHT_API bool TryParseStackScriptUsageAlias(const FString& ScriptUsage, ENiagaraScriptUsage& OutUsage);
    PINWRIGHT_API FString StackScriptUsageToString(ENiagaraScriptUsage Usage);
    PINWRIGHT_API FNiagaraEditError RejectBatchPayload(const TSharedPtr<FJsonObject>& Payload);

    PINWRIGHT_API FNiagaraEditError ParsePropertyPayload(
        const TSharedPtr<FJsonObject>& Payload,
        FNiagaraPropertyEditPayload& OutPayload);
    PINWRIGHT_API FNiagaraEditError ParseParameterPayload(
        const TSharedPtr<FJsonObject>& Payload,
        ENiagaraEditOperation Operation,
        FNiagaraParameterEditPayload& OutPayload);
    PINWRIGHT_API FNiagaraEditError ParseRendererPayload(
        const TSharedPtr<FJsonObject>& Payload,
        ENiagaraEditOperation Operation,
        FNiagaraRendererEditPayload& OutPayload);
    // ParseRendererPayload plus the top-level `index` read and the ordinal required-param checks.
    // Only niagara.remove_renderer / niagara.move_renderer may call it -- niagara.add_renderer does
    // not declare `index` and never reads Target.Index, so the read stays out of the shared parser.
    PINWRIGHT_API FNiagaraEditError ParseRendererOrdinalPayload(
        const TSharedPtr<FJsonObject>& Payload,
        ENiagaraEditOperation Operation,
        FNiagaraRendererEditPayload& OutPayload);
    PINWRIGHT_API FNiagaraEditError ParseModulePayload(
        const TSharedPtr<FJsonObject>& Payload,
        ENiagaraEditOperation Operation,
        FNiagaraModuleEditPayload& OutPayload);
    PINWRIGHT_API FNiagaraEditError ParsePinPayload(
        const TSharedPtr<FJsonObject>& Payload,
        ENiagaraEditOperation Operation,
        FNiagaraPinEditPayload& OutPayload);

    // Compose the owner-qualified module entry key "<ownerName>:<nodeGuid>" that the stack
    // dump emits as `entryKey` beside `entryId`. A bare `entryId` is the raw
    // UEdGraphNode::NodeGuid, and duplicating an emitter copies its graph — NodeGuids
    // included — so the same id identifies a different module in every emitter descended from
    // one template. The qualified form is the only addressing key that survives being stored
    // and replayed (B-niagara-entry-id-not-unique-across-emitters).
    PINWRIGHT_API FString MakeModuleEntryKey(const FString& OwnerName, const FGuid& NodeGuid);

    // Split an owner-qualified entry key back into owner and id. Returns true only when the
    // text after the last ':' parses as a Guid, so a bare guid, a node name, or a node title
    // carrying a colon is left untouched.
    PINWRIGHT_API bool TrySplitModuleEntryKey(
        const FString& EntryKey,
        FString& OutOwnerName,
        FString& OutEntryId);

    PINWRIGHT_API FNiagaraEditError ResolveTarget(
        const FString& AssetPath,
        const FNiagaraEditTargetSpec& TargetSpec,
        FNiagaraResolvedTarget& OutTarget);

    PINWRIGHT_API FNiagaraEditError ValidatePropertyPayload(
        const FNiagaraPropertyEditPayload& Payload,
        FNiagaraResolvedTarget& OutTarget,
        FProperty*& OutProperty,
        void*& OutContainer);
    PINWRIGHT_API FNiagaraEditError ValidateParameterPayload(
        const FNiagaraParameterEditPayload& Payload,
        ENiagaraEditOperation Operation,
        FNiagaraResolvedTarget& OutTarget);
    PINWRIGHT_API FNiagaraEditError ValidateRendererPayload(
        const FNiagaraRendererEditPayload& Payload,
        ENiagaraEditOperation Operation,
        FNiagaraResolvedTarget& OutTarget);
    PINWRIGHT_API FNiagaraEditError ValidateModulePayload(
        const FNiagaraModuleEditPayload& Payload,
        ENiagaraEditOperation Operation,
        FNiagaraResolvedTarget& OutTarget);
    PINWRIGHT_API FNiagaraEditError ValidatePinPayload(
        const FNiagaraPinEditPayload& Payload,
        ENiagaraEditOperation Operation,
        FNiagaraResolvedTarget& OutTarget);

    PINWRIGHT_API FNiagaraEditError ParseEventHandlerPayload(
        const TSharedPtr<FJsonObject>& Payload,
        bool bRequireIdentity,
        FNiagaraEventHandlerEditPayload& OutPayload);
    PINWRIGHT_API FNiagaraEditError ParseSimulationStagePayload(
        const TSharedPtr<FJsonObject>& Payload,
        bool bRequireIdentity,
        FNiagaraSimulationStageEditPayload& OutPayload);
    PINWRIGHT_API FNiagaraEditError ParseDataInterfacePayload(
        const TSharedPtr<FJsonObject>& Payload,
        bool bRequireClass,
        FNiagaraDataInterfaceEditPayload& OutPayload);

    PINWRIGHT_API UClass* ResolveNiagaraSubclassByPath(
        UClass* BaseClass,
        const FString& ClassPath,
        const TCHAR* DefaultModule);

    template <typename TBase>
    UClass* ResolveNiagaraSubclass(const FString& ClassPath, const TCHAR* DefaultModule = TEXT("Niagara"))
    {
        return ResolveNiagaraSubclassByPath(TBase::StaticClass(), ClassPath, DefaultModule);
    }

    // Measures Target's compiled-vs-resolved data-interface verdict and records it as the BEFORE
    // half of the delta MakeMutationResult publishes. Idempotent: the first measurement of a call
    // wins, so a verb that passes through two mutation seams still reports the state it found
    // rather than the state its own first step created.
    //
    // Must run before anything mutates the target. The two seams that qualify are
    // ModifyResolvedTarget and NiagaraJsonHelpers::BeginEmitterMutationScope, both of which call
    // it, so a verb that opens its transaction through either is covered without a per-handler
    // line. A verb that reaches FinalizeNiagaraEdit through neither reports the delta as
    // unmeasured rather than guessing.
    PINWRIGHT_API void RecordDataInterfaceVerdictBefore(FNiagaraResolvedTarget& Target);

    // Shared mutation helpers used by both `NiagaraEditHandler` and
    // `NiagaraAdvancedEditHandler`. Each operates on a resolved target.
    //
    // Non-const because it is one of the two pre-mutation seams that records the before-verdict.
    PINWRIGHT_API void ModifyResolvedTarget(FNiagaraResolvedTarget& Target);

    // Centralizes emitter version-guid resolution so future emitter-versioning changes touch one site.
    PINWRIGHT_API FGuid ResolveEmitterVersionGuid(const FNiagaraResolvedTarget& Target);
    PINWRIGHT_API void NotifyNiagaraObjectChanged(const FNiagaraResolvedTarget& Target, FProperty* ChangedProperty);

    // Issue a compile for an already-resolved target, quiescing that target's live system
    // instances first. Asynchronous — a true return means a compile was REQUESTED, never that
    // one finished. Target.CompileRequestSystems records exactly the systems whose requests
    // launched. A system target can pass that set to WaitForRequestedSystemCompiles; an emitter
    // target must pass it to WaitForEmitterCompile so older active compiles are observed too.
    //
    // Shared by FinalizeNiagaraEdit, which always forces because it runs directly after a
    // mutation, and by `niagara.compile`, which honours the caller's `force`. Returns false
    // when the engine launched no compile (including a current system under force:false, a
    // source-less transient system, and an emitter used by no source-valid loaded system), and
    // target resolves to neither a system nor an emitter.
    //
    // Target is non-const because quiesce and the actual request set are recorded on it.
    PINWRIGHT_API bool RequestNiagaraCompile(FNiagaraResolvedTarget& Target, bool bForce);

    // Whether a system carrying this data-interface verdict may be written to disk. The
    // counterpart of PinWrightNiagara::MayPersistAfterCompileWait, and the same shape of gate:
    // only Mismatched refuses. Unverified must NOT refuse — the check needs a resolved
    // data-interface set to compare, and treating "could not look" as "corrupt" would block every
    // legitimate write to a system that has not compiled this session, and every edit addressed at
    // a standalone Niagara Emitter asset (B-niagara-finalize-edit-no-di-gate).
    PINWRIGHT_API bool MayPersistAfterDataInterfaceCheck(PinWrightNiagara::EDataInterfaceConsistency Verdict);

    // Stable wire spelling for FNiagaraResolvedTarget::EDataInterfaceRepair.
    PINWRIGHT_API const TCHAR* DataInterfaceRepairToString(FNiagaraResolvedTarget::EDataInterfaceRepair Repair);

    // Publishes `dataInterfaceDelta`: the verdict as this call FOUND the system and the verdict as
    // it LEFT it, plus whether the difference is this call's doing.
    //
    // Echoing only the state a write ends in cannot answer the question a caller actually has -
    // "did I just do this?" - and every mutating verb in this namespace answered it with silence.
    // `before` reads "unmeasured" when the verb reached the response without passing a
    // pre-mutation seam; that is not the same claim as "unverified" and must not be collapsed
    // into it. Shared by NiagaraEdit::MakeMutationResult and the two emitter-handle verbs in
    // NiagaraHandler.cpp so one fact keeps one spelling across the namespace.
    PINWRIGHT_API void AddDataInterfaceDelta(
        const TSharedPtr<FJsonObject>& Result,
        bool bBeforeMeasured,
        PinWrightNiagara::EDataInterfaceConsistency Before,
        PinWrightNiagara::EDataInterfaceConsistency After,
        FNiagaraResolvedTarget::EDataInterfaceRepair Repair = FNiagaraResolvedTarget::EDataInterfaceRepair::NotNeeded,
        int32 LiveInstancesAtRepair = 0);

    // Use the shared AssetSaveState response shape, while making pendingFlush precise for Niagara:
    // only Deferred is flushable; terminal/refused states publish pendingFlush:false and their
    // saveDetail remedy.
    PINWRIGHT_API void AddNiagaraAssetSaveReport(
        const TSharedPtr<FJsonObject>& Result,
        bool bSaveRequested,
        bool bSavedToDisk,
        EAssetSaveState SaveState);

    // Applies the requested compile and save to an edited target. When BOTH are requested the
    // compile is waited out before the save, and the save is refused unless an actual engine
    // request lands with nothing outstanding. Compile-only stays asynchronous and therefore
    // reports compiled:false; niagara.compile_status is the completion readback.
    //
    // Then, after that wait and before the write, measures the target system's compiled-vs-resolved
    // data-interface counts into Target.DataInterfaceVerdict / Target.DataInterfaceMismatches and
    // refuses the save on a mismatch, because persisting that state is what arms the VectorVM
    // assert that kills the editor on the asset's next tick. Ordering is load-bearing: a landed
    // compile is what clears a mismatch, so the check has to follow the wait.
    PINWRIGHT_API bool FinalizeNiagaraEdit(FNiagaraResolvedTarget& Target, const FNiagaraEditOptions& Options, bool& bOutCompiled, bool& bOutSaved);

    // Standard success envelope for every mutation verb. Also reports `quiescedInstances`:
    // how many running system instances this call stopped, which is the only signal a caller
    // whose preview viewport went blank gets for why. And `dataInterfaceCheck` — always, including
    // `"unverified"`, so a check that could not run is never echoed as one that passed — plus
    // `mismatchedScripts` when it did not. Persistence uses the shared saveRequested / saved /
    // pendingFlush / saveState / saveDetail shape; pendingFlush is true only for Deferred.
    PINWRIGHT_API TSharedPtr<FJsonObject> MakeMutationResult(
        const TCHAR* Operation,
        const FNiagaraResolvedTarget& Target,
        const FNiagaraEditOptions& Options,
        bool bCompiled,
        bool bSaved);

    // set_module_script helpers — exposed for unit tests via NiagaraSetModuleScriptHelpers.h
    PINWRIGHT_API void SnapshotInputOverrides(
        UNiagaraNodeFunctionCall& ModuleNode,
        TMap<FNiagaraVariable, FString>& OutSnapshot);

    PINWRIGHT_API void EnumerateScriptInputs(
        UNiagaraScript& Script,
        TArray<FNiagaraVariable>& OutInputs);

    // Enumerate a PLACED module's stack inputs (the "Module." namespace inputs the stack UI
    // shows, e.g. SpawnRate) as declared FNiagaraVariable name+type. Unlike
    // EnumerateScriptInputs — which surfaces only the script's ParameterMap input node — this
    // resolves the module-namespace reads via FNiagaraStackGraphUtilities::GetStackFunctionInputs,
    // the same enumeration the Niagara stack view uses. Returned names carry the "Module."
    // namespace; callers strip it via FNiagaraParameterHandle for the short input name.
    PINWRIGHT_API void EnumerateModuleStackInputs(
        const UNiagaraNodeFunctionCall& ModuleNode,
        TArray<FNiagaraVariable>& OutInputs);

    // Current binding of one placed-module stack input, for schema readback.
    struct FModuleInputBindingInfo
    {
        // How the input is currently driven (classified as the engine's
        // UNiagaraStackFunctionInput::UpdateValuesFromOverridePin does):
        //   "local"        — a literal override value written on the override pin
        //   "linked"       — bound to a parameter (a UNiagaraNodeParameterMapGet override)
        //   "data"         — a data-interface value (a UNiagaraNodeInput override)
        //   "objectAsset"  — an object-asset value (a UNiagaraNodeInput override)
        //   "expression"   — an inline custom-HLSL expression (a UNiagaraNodeCustomHlsl override)
        //   "dynamicInput" — driven by a dynamic-input function-call chain
        //   "connected"    — override pin wired to some other upstream node
        // Inputs with no override entry at all are reported by the caller as "default".
        FString ValueMode;
        FString LiteralValue;       // set when ValueMode == "local"
        FString LinkedParameter;    // set when ValueMode == "linked"
        FString DynamicInputScript; // object path, set when ValueMode == "dynamicInput"
    };

    // Classify each of ModuleNode's stack-input overrides by walking the module's
    // override node, keyed by the input's short name. Mirrors SnapshotInputOverrides'
    // override-node walk but distinguishes linked-parameter and dynamic-input
    // connections from literal values. Inputs absent from OutByName are at their
    // script default. Reused by the niagara.inspect / asset.dump stack readback so a
    // per-module typed input schema can report value modes without re-walking.
    PINWRIGHT_API void ClassifyModuleInputBindings(
        UNiagaraNodeFunctionCall& ModuleNode,
        TMap<FName, FModuleInputBindingInfo>& OutByName);

    // The value a module input falls back to when nothing overrides it, as the module script
    // graph's UNiagaraScriptVariable declares it — what the Niagara stack UI shows greyed-out
    // on a row nobody wrote.
    struct FModuleInputDefaultInfo
    {
        // ENiagaraDefaultMode as text: "Value", "Binding", "Custom", "FailIfPreviouslyNotSet".
        FString DefaultMode;
        // Canonical pin-default string, in the same encoding FModuleInputBindingInfo::LiteralValue
        // carries, so a stated and a defaulted value are diffable without a type-aware branch.
        // Empty unless DefaultMode is "Value" and the declaration carries allocated data.
        FString DefaultValue;
        // Bound attribute (e.g. "Particles.Age"). Empty unless DefaultMode is "Binding".
        FString DefaultBinding;
    };

    // Describe one script variable's declared default. Separate from the graph lookup below so
    // the decode is reachable without a module graph.
    PINWRIGHT_API bool DescribeScriptVariableDefault(
        const UNiagaraScriptVariable& ScriptVariable,
        FModuleInputDefaultInfo& OutInfo);

    // Resolve ModuleInput's declared default off the module script graph's metadata map. The
    // MapGet default pins that hold the same numbers are anonymous and are paired to their
    // output pins only through PinOutputToPinDefaultPersistentId, an id space no readback
    // exposes; the script variable is the authoritative and joinable source.
    PINWRIGHT_API bool ResolveModuleInputDefault(
        const UNiagaraGraph* ModuleGraph,
        const FNiagaraVariable& ModuleInput,
        FModuleInputDefaultInfo& OutInfo);

    // The static switch branch that strands a module input: the input's only consumers sit on
    // a branch the switch does not currently take, so a write to it is dead code.
    struct FModuleInputGate
    {
        // The switch's InputParameterName, as staticSwitchInputs[] publishes it.
        FName SwitchName;
        // The switch's value today, in the same JSON encoding staticSwitchInputs[].value uses.
        TSharedPtr<FJsonValue> CurrentValue;
        // The value that would route this input into the graph, same encoding.
        TSharedPtr<FJsonValue> RequiredValue;
        // Label of the branch the switch takes today ("false", "3", an enum display name).
        FString BranchTaken;
    };

    // Report which of DeclaredInputs a static switch currently strands, keyed by short input
    // name. Conservative by construction: an input is reported only when every consumer its
    // reads reach is a static-switch branch pin that the switch's current value does not
    // select, so an unrecognised graph shape reads as reachable rather than as a false alarm.
    PINWRIGHT_API void ClassifyModuleInputReachability(
        const UNiagaraNodeFunctionCall& ModuleNode,
        const TArray<FNiagaraVariable>& DeclaredInputs,
        TMap<FName, FModuleInputGate>& OutGatedByName);

    // Single-input form for the write path, so set_module_input can say at write time that the
    // value it just stored is unreachable. Returns true when the input is gated.
    PINWRIGHT_API bool FindModuleInputGate(
        const UNiagaraNodeFunctionCall& ModuleNode,
        FName ShortInputName,
        FModuleInputGate& OutGate);

    // The `gatedBy` object shared by niagara.inspect's moduleInputs[] and the
    // set_module_input response, so a readback and a write cannot describe a gate differently.
    PINWRIGHT_API TSharedPtr<FJsonObject> MakeGatedByJson(const FModuleInputGate& Gate);
}

namespace NiagaraStaticSwitch
{
    // Locate a static switch declaration by its input parameter name within a called graph.
    PINWRIGHT_API bool FindByName(
        const UNiagaraGraph* CalledGraph,
        FName InputName,
        const UNiagaraNodeStaticSwitch*& OutDecl);

    // One branch an enum static switch offers, addressed the way a caller addresses it.
    // Index is the selector Niagara's own FNiagaraEditorUtilities::ResolveConstantValue
    // derives from the caller-pin default (GenerateFullEnumName + GetIndexByName), so it is
    // the integer the value parameter takes. Name is the authored entry name — NewEnumeratorN
    // on the user-defined enums Niagara's stock modules use — and DisplayName is the label the
    // Niagara editor shows. On a user-defined enum the three are unrelated: the entry order is
    // a permutation of the name order, so neither the index nor the label can be derived from
    // the other without this table.
    struct FEnumSwitchOption
    {
        int32 Index = INDEX_NONE;
        FString Name;
        FString DisplayName;
    };

    // Enumerate the branches an enum static switch offers, mirroring the filter in
    // UNiagaraNodeStaticSwitch::GetOptionValues: the trailing _MAX sentinel and any entry
    // marked Hidden or Spacer are not branches and are left out.
    PINWRIGHT_API void BuildEnumOptions(const UEnum* Enum, TArray<FEnumSwitchOption>& OutOptions);

    // The branch table as JSON ({index, name, displayName}), so a caller can read the
    // integer <-> label mapping off a response instead of probing one integer at a time.
    PINWRIGHT_API TArray<TSharedPtr<FJsonValue>> MakeEnumOptionsJson(const UEnum* Enum);

    // Resolve a JSON value to one enum branch. A number is the branch index and is
    // range-checked against the table; a string matches the authored entry name, its fully
    // qualified form, or the display name the editor shows, compared case-insensitively and
    // ignoring spaces. On failure OutError names the rejected input and lists the whole table.
    PINWRIGHT_API bool ResolveEnumOption(
        const UEnum* Enum,
        const TSharedPtr<FJsonValue>& Json,
        FEnumSwitchOption& OutOption,
        FString& OutError);

    // Integer static switches declare a contiguous branch domain on the node. Validate against
    // that declaration instead of allowing Niagara to compile an out-of-range selector as an
    // unintended branch. OutError names the valid range when the value is rejected.
    PINWRIGHT_API bool ValidateIntegerOption(
        const UNiagaraNodeStaticSwitch& SwitchDecl,
        int32 Value,
        FString& OutError);

    // Decode a caller-pin DefaultValue string into a JSON value matching the switch's type.
    // Fails when an enum switch's stored name is not an entry of its enum (or the enum class is
    // gone): a stored name that resolves to nothing is not branch 0, and reporting it as 0 is
    // indistinguishable from a genuine branch-0 override. OutError, when supplied, names the
    // rejected string and lists the branch table.
    PINWRIGHT_API bool DecodePinDefault(
        const FString& PinDefault,
        ENiagaraStaticSwitchType Type,
        const UEnum* Enum,
        TSharedPtr<FJsonValue>& OutValue,
        FString* OutError = nullptr);

    // Encode a JSON value into the caller-pin DefaultValue string format the switch expects.
    // For an enum switch OutEnumOption, when supplied, receives the branch that was selected,
    // so the caller can echo the index and label it wrote without re-resolving.
    PINWRIGHT_API bool EncodePinDefault(
        const TSharedPtr<FJsonValue>& Json,
        ENiagaraStaticSwitchType Type,
        const UEnum* Enum,
        FString& OutPinDefault,
        FString& OutError,
        FEnumSwitchOption* OutEnumOption = nullptr);
}
