// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/Niagara/NiagaraEditTypes.h"

#include "Handlers/Blueprint/BlueprintEnumHelpers.h"
#include "Handlers/Niagara/NiagaraCompileWait.h"
#include "Handlers/Niagara/NiagaraDataInterfaceConsistency.h"
#include "Handlers/Niagara/NiagaraEditorOpenGuard.h"
#include "Handlers/Niagara/NiagaraInstanceUtils.h"
#include "Handlers/Niagara/NiagaraJsonHelpers.h"
#include "Handlers/Niagara/NiagaraParameterTypeResolver.h"
#include "PinWrightSubsystem.h"
#include "Utils/PropertyUtils.h"

#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "NiagaraDataInterface.h"
#include "NiagaraEmitter.h"
#include "NiagaraEmitterHandle.h"
#include "EdGraphSchema_Niagara.h"
#include "NiagaraGraph.h"
#include "NiagaraNode.h"
#include "NiagaraNodeCustomHlsl.h"
#include "NiagaraNodeFunctionCall.h"
#include "NiagaraNodeInput.h"
#include "NiagaraNodeStaticSwitch.h"
#include "ViewModels/Stack/NiagaraParameterHandle.h"
#include "ViewModels/Stack/NiagaraStackGraphUtilities.h"
#include "NiagaraParameterMapHistory.h"
#include "NiagaraNodeParameterMapSet.h"
#include "Handlers/Niagara/NiagaraResetModuleInputHelpers.h"
#include "NiagaraParameterStore.h"
#include "NiagaraRendererProperties.h"
#include "NiagaraScript.h"
#include "NiagaraScriptSource.h"
#include "NiagaraScriptVariable.h"
#include "NiagaraSimulationStageBase.h"
#include "NiagaraSystem.h"
#include "NiagaraConstants.h"
// Compat header, not the raw engine one: UE 5.3 does not define UE_VERSION_NEWER_THAN_OR_EQUAL.
#include "Compat/EngineVersionCompat.h"
#include "NiagaraTypes.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UObjectIterator.h"

#include "Utils/AssetUtils.h"

#include <initializer_list>

FNiagaraEditError FNiagaraEditError::Make(const TCHAR* InCode, const FString& InMessage)
{
    FNiagaraEditError Error;
    Error.Code = InCode;
    Error.Message = InMessage;
    return Error;
}

namespace
{
    FString GetStringField(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field, const FString& Default = FString())
    {
        FString Value;
        if (Object.IsValid() && Object->TryGetStringField(Field, Value))
        {
            return Value;
        }
        return Default;
    }

    bool HasField(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field)
    {
        return Object.IsValid() && Object->HasField(Field);
    }

    TSharedPtr<FJsonValue> GetFieldValue(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field)
    {
        return Object.IsValid() ? Object->TryGetField(Field) : nullptr;
    }

    bool TryGetIntField(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field, int32& OutValue)
    {
        if (!Object.IsValid())
        {
            return false;
        }

        double NumberValue = 0.0;
        if (Object->TryGetNumberField(Field, NumberValue))
        {
            OutValue = static_cast<int32>(NumberValue);
            return true;
        }
        return false;
    }

    FNiagaraEditOptions ParseOptions(const TSharedPtr<FJsonObject>& Payload)
    {
        FNiagaraEditOptions Options;
        if (Payload.IsValid())
        {
            Payload->TryGetBoolField(TEXT("compile"), Options.bCompile);
            Payload->TryGetBoolField(TEXT("save"), Options.bSave);
        }
        return Options;
    }

    ENiagaraEditTargetKind ParseTargetKind(const FString& Kind)
    {
        // Lower-case keys; lookup lowercases the input. Initialized once on first call.
        static const TMap<FString, ENiagaraEditTargetKind> Aliases = []()
        {
            TMap<FString, ENiagaraEditTargetKind> Map;
            Map.Add(TEXT("system"), ENiagaraEditTargetKind::System);
            Map.Add(TEXT("emitter"), ENiagaraEditTargetKind::EmitterHandle);
            Map.Add(TEXT("emitterhandle"), ENiagaraEditTargetKind::EmitterHandle);
            Map.Add(TEXT("emitter_handle"), ENiagaraEditTargetKind::EmitterHandle);
            Map.Add(TEXT("emitterdata"), ENiagaraEditTargetKind::EmitterData);
            Map.Add(TEXT("emitter_data"), ENiagaraEditTargetKind::EmitterData);
            Map.Add(TEXT("versionedemitterdata"), ENiagaraEditTargetKind::EmitterData);
            Map.Add(TEXT("renderer"), ENiagaraEditTargetKind::Renderer);
            Map.Add(TEXT("parameterstore"), ENiagaraEditTargetKind::ParameterStore);
            Map.Add(TEXT("parameter_store"), ENiagaraEditTargetKind::ParameterStore);
            Map.Add(TEXT("graph"), ENiagaraEditTargetKind::Graph);
            Map.Add(TEXT("node"), ENiagaraEditTargetKind::Node);
            Map.Add(TEXT("pin"), ENiagaraEditTargetKind::Pin);
            Map.Add(TEXT("datainterface"), ENiagaraEditTargetKind::DataInterface);
            Map.Add(TEXT("data_interface"), ENiagaraEditTargetKind::DataInterface);
            Map.Add(TEXT("module"), ENiagaraEditTargetKind::Module);
            Map.Add(TEXT("functioncall"), ENiagaraEditTargetKind::Module);
            Map.Add(TEXT("function_call"), ENiagaraEditTargetKind::Module);
            Map.Add(TEXT("eventhandler"), ENiagaraEditTargetKind::EventHandler);
            Map.Add(TEXT("event_handler"), ENiagaraEditTargetKind::EventHandler);
            Map.Add(TEXT("simulationstage"), ENiagaraEditTargetKind::SimulationStage);
            Map.Add(TEXT("simulation_stage"), ENiagaraEditTargetKind::SimulationStage);
            return Map;
        }();
        const FString Key = Kind.ToLower();
        if (const ENiagaraEditTargetKind* Found = Aliases.Find(Key))
        {
            return *Found;
        }
        return ENiagaraEditTargetKind::Unknown;
    }

    FNiagaraEditError ParseTargetSpec(
        const TSharedPtr<FJsonObject>& Payload,
        ENiagaraEditTargetKind DefaultKind,
        FNiagaraEditTargetSpec& OutTarget)
    {
        TSharedPtr<FJsonObject> TargetObject;
        const TSharedPtr<FJsonObject>* TargetObjectPtr = nullptr;
        if (Payload.IsValid() && Payload->TryGetObjectField(TEXT("target"), TargetObjectPtr) && TargetObjectPtr)
        {
            TargetObject = *TargetObjectPtr;
        }

        FString KindText = GetStringField(TargetObject, TEXT("kind"));
        if (KindText.IsEmpty())
        {
            KindText = GetStringField(Payload, TEXT("targetKind"));
        }

        OutTarget.Kind = KindText.IsEmpty() ? DefaultKind : ParseTargetKind(KindText);
        OutTarget.KindText = KindText.IsEmpty() ? NiagaraEdit::TargetKindToString(DefaultKind) : KindText;
        if (OutTarget.Kind == ENiagaraEditTargetKind::Unknown)
        {
            return FNiagaraEditError::Make(
                TEXT("INVALID_TARGET_KIND"),
                FString::Printf(TEXT("Unknown Niagara edit target kind '%s'."), *KindText));
        }

        OutTarget.EmitterName = GetStringField(TargetObject, TEXT("emitter"));
        if (OutTarget.EmitterName.IsEmpty())
        {
            OutTarget.EmitterName = GetStringField(TargetObject, TEXT("emitterName"));
        }
        if (OutTarget.EmitterName.IsEmpty())
        {
            OutTarget.EmitterName = GetStringField(Payload, TEXT("emitter"));
        }
        if (OutTarget.EmitterName.IsEmpty())
        {
            OutTarget.EmitterName = GetStringField(Payload, TEXT("emitterName"));
        }

        // Nested only, deliberately. FNiagaraEditTargetSpec::Scope is read by ResolveParameterStore
        // and nothing else, and every verb that reaches it (set/add/remove_parameter,
        // add/remove_data_interface, set_curve_keys, bind_curve_asset) builds its target spec by
        // hand from the `scope` its OWN parser read -- none of them routes through ParseTargetSpec.
        // The top-level fallback that used to sit here therefore fed nobody, while handing every
        // module / pin / renderer / property verb a `scope` key its RPC_PARAMS does not declare, so
        // FRpcDispatcher::ValidateHandlerParams refused those callers with UNKNOWN_PARAMS before
        // the body ran.
        OutTarget.Scope = GetStringField(TargetObject, TEXT("scope"));
        OutTarget.ScriptUsage = GetStringField(TargetObject, TEXT("scriptUsage"), GetStringField(Payload, TEXT("scriptUsage")));
        if (OutTarget.ScriptUsage.IsEmpty())
        {
            OutTarget.ScriptUsage = GetStringField(TargetObject, TEXT("scriptType"), GetStringField(Payload, TEXT("scriptType")));
        }

        OutTarget.NodeId = GetStringField(TargetObject, TEXT("nodeId"), GetStringField(Payload, TEXT("nodeId")));
        if (OutTarget.NodeId.IsEmpty())
        {
            OutTarget.NodeId = GetStringField(TargetObject, TEXT("node"), GetStringField(Payload, TEXT("node")));
        }
        OutTarget.PinName = GetStringField(TargetObject, TEXT("pin"), GetStringField(Payload, TEXT("pin")));
        if (OutTarget.PinName.IsEmpty())
        {
            OutTarget.PinName = GetStringField(TargetObject, TEXT("pinName"), GetStringField(Payload, TEXT("pinName")));
        }
        OutTarget.EntryId = GetStringField(TargetObject, TEXT("entryId"), GetStringField(Payload, TEXT("entryId")));
        if (OutTarget.EntryId.IsEmpty())
        {
            OutTarget.EntryId = GetStringField(TargetObject, TEXT("moduleId"), GetStringField(Payload, TEXT("moduleId")));
        }
        // Accept the owner-qualified `entryKey` form ("<ownerName>:<nodeGuid>") wherever a bare
        // `entryId` is accepted, and carry the owner separately so ResolveTarget can both select
        // and cross-check the emitter it names.
        {
            FString QualifiedOwner;
            FString QualifiedEntryId;
            if (NiagaraEdit::TrySplitModuleEntryKey(OutTarget.EntryId, QualifiedOwner, QualifiedEntryId))
            {
                OutTarget.EntryOwner = QualifiedOwner;
                OutTarget.EntryId = QualifiedEntryId;
            }
        }

        TryGetIntField(TargetObject, TEXT("index"), OutTarget.Index);
        // The top-level `index` spelling is read by ParseRendererOrdinalPayload, called only by the
        // two verbs that declare it and address their target by ordinal (remove_renderer /
        // move_renderer). Reading it here gave it to every module, pin, property and add_* verb too,
        // none of which declares it or ever looks at TargetSpec.Index, so those callers were refused
        // with UNKNOWN_PARAMS.
        TryGetIntField(Payload, TEXT("rendererIndex"), OutTarget.Index);
        TryGetIntField(TargetObject, TEXT("toIndex"), OutTarget.ToIndex);
        TryGetIntField(Payload, TEXT("toIndex"), OutTarget.ToIndex);
        return FNiagaraEditError();
    }

    bool IsObjectWithNumbers(const TSharedPtr<FJsonValue>& Value, std::initializer_list<const TCHAR*> Fields)
    {
        if (!Value.IsValid() || Value->Type != EJson::Object)
        {
            return false;
        }
        TSharedPtr<FJsonObject> Object = Value->AsObject();
        for (const TCHAR* Field : Fields)
        {
            double Number = 0.0;
            if (!Object.IsValid() || !Object->TryGetNumberField(Field, Number))
            {
                return false;
            }
        }
        return true;
    }

    bool IsArrayWithLength(const TSharedPtr<FJsonValue>& Value, int32 Length)
    {
        if (!Value.IsValid() || Value->Type != EJson::Array)
        {
            return false;
        }
        const TArray<TSharedPtr<FJsonValue>>& Array = Value->AsArray();
        if (Array.Num() < Length)
        {
            return false;
        }
        for (int32 Index = 0; Index < Length; ++Index)
        {
            if (!Array[Index].IsValid() || Array[Index]->Type != EJson::Number)
            {
                return false;
            }
        }
        return true;
    }

    // The wire `type` and the value shape it demands are decided from ONE resolution, so the set
    // of accepted spellings and the shape rule cannot disagree. They used to: this function keyed
    // off the raw string and fell through to "is it a registered script struct?", which is true of
    // NiagaraInt32 and NiagaraFloat as much as of NiagaraSpawnInfo — so the canonical name
    // niagara.inspect prints was told a scalar int needed a JSON object, while short aliases the
    // writer supported ("half", "vec2") were refused as unknown types.
    FNiagaraEditError ValidateTypedValue(const FString& Type, const TSharedPtr<FJsonValue>& Value, const TCHAR* FieldName)
    {
        if (!Value.IsValid())
        {
            return FNiagaraEditError::Make(TEXT("INVALID_ARGUMENT"), FString::Printf(TEXT("Missing '%s'."), FieldName));
        }

        PinWrightNiagara::FNiagaraResolvedParameterType ResolvedType;
        if (!PinWrightNiagara::ResolveNiagaraParameterType(Type, ResolvedType))
        {
            return FNiagaraEditError::Make(
                TEXT("INVALID_PARAMETER_TYPE"),
                FString::Printf(
                    TEXT("Unsupported Niagara parameter type '%s'. Accepted: %s, or any registered Niagara type by the name niagara.inspect prints or by its struct path."),
                    *Type,
                    *FString::Join(PinWrightNiagara::GetNiagaraParameterTypeAliases(), TEXT(", "))));
        }

        using EValueKind = PinWrightNiagara::ENiagaraParameterValueKind;
        switch (ResolvedType.Kind)
        {
        case EValueKind::Float:
        case EValueKind::Int:
        case EValueKind::Half:
            return Value->Type == EJson::Number
                ? FNiagaraEditError()
                : FNiagaraEditError::Make(TEXT("INVALID_VALUE"), FString::Printf(TEXT("Parameter type '%s' requires a numeric value."), *Type));

        case EValueKind::Bool:
            return Value->Type == EJson::Boolean
                ? FNiagaraEditError()
                : FNiagaraEditError::Make(TEXT("INVALID_VALUE"), FString::Printf(TEXT("Parameter type '%s' requires a boolean value."), *Type));

        case EValueKind::LinearColor:
            return IsObjectWithNumbers(Value, {TEXT("r"), TEXT("g"), TEXT("b")}) || IsArrayWithLength(Value, 3)
                ? FNiagaraEditError()
                : FNiagaraEditError::Make(TEXT("INVALID_VALUE"), TEXT("Color parameters require {r,g,b[,a]} or [r,g,b,a]."));

        case EValueKind::Vec2:
        case EValueKind::HalfVec2:
            return IsObjectWithNumbers(Value, {TEXT("x"), TEXT("y")}) || IsArrayWithLength(Value, 2)
                ? FNiagaraEditError()
                : FNiagaraEditError::Make(TEXT("INVALID_VALUE"), FString::Printf(TEXT("Parameter type '%s' requires {x,y} or [x,y]."), *Type));

        case EValueKind::Vec3:
        case EValueKind::Position:
        case EValueKind::HalfVec3:
            return IsObjectWithNumbers(Value, {TEXT("x"), TEXT("y"), TEXT("z")}) || IsArrayWithLength(Value, 3)
                ? FNiagaraEditError()
                : FNiagaraEditError::Make(TEXT("INVALID_VALUE"), TEXT("Vector parameters require {x,y,z} or [x,y,z]."));

        case EValueKind::HalfVec4:
            return IsObjectWithNumbers(Value, {TEXT("x"), TEXT("y"), TEXT("z"), TEXT("w")}) || IsArrayWithLength(Value, 4)
                ? FNiagaraEditError()
                : FNiagaraEditError::Make(TEXT("INVALID_VALUE"), FString::Printf(TEXT("Parameter type '%s' requires {x,y,z,w} or [x,y,z,w]."), *Type));

        case EValueKind::NiagaraID:
            return IsObjectWithNumbers(Value, {TEXT("index"), TEXT("acquireTag")}) || IsArrayWithLength(Value, 2)
                ? FNiagaraEditError()
                : FNiagaraEditError::Make(TEXT("INVALID_VALUE"), TEXT("NiagaraID parameters require {index,acquireTag} or [index,acquireTag]."));

        case EValueKind::NiagaraSpawnInfo:
        case EValueKind::ScriptStruct:
        default:
            // Written field by field onto the struct, so the value has to be an object.
            return Value->Type == EJson::Object
                ? FNiagaraEditError()
                : FNiagaraEditError::Make(TEXT("INVALID_VALUE"), FString::Printf(TEXT("Parameter type '%s' requires a JSON object value."), *Type));
        }
    }

    bool ScriptUsageMatches(const FString& ScriptUsage, const TCHAR* A, const TCHAR* B)
    {
        return ScriptUsage.Equals(A, ESearchCase::IgnoreCase)
            || ScriptUsage.Equals(B, ESearchCase::IgnoreCase)
            || ScriptUsage.EndsWith(A, ESearchCase::IgnoreCase)
            || ScriptUsage.EndsWith(B, ESearchCase::IgnoreCase);
    }

    bool ScriptUsageRequiresEmitter(const FString& ScriptUsage)
    {
        return ScriptUsageMatches(ScriptUsage, TEXT("EmitterSpawn"), TEXT("EmitterSpawnScript"))
            || ScriptUsageMatches(ScriptUsage, TEXT("EmitterUpdate"), TEXT("EmitterUpdateScript"))
            || ScriptUsageMatches(ScriptUsage, TEXT("ParticleSpawn"), TEXT("ParticleSpawnScript"))
            || ScriptUsageMatches(ScriptUsage, TEXT("ParticleUpdate"), TEXT("ParticleUpdateScript"));
    }

    UNiagaraScript* ResolveScript(
        UNiagaraSystem* System,
        FVersionedNiagaraEmitterData* EmitterData,
        const FString& ScriptUsage)
    {
        if (EmitterData)
        {
            if (ScriptUsageMatches(ScriptUsage, TEXT("EmitterSpawn"), TEXT("EmitterSpawnScript")))
            {
                return EmitterData->EmitterSpawnScriptProps.Script;
            }
            if (ScriptUsageMatches(ScriptUsage, TEXT("EmitterUpdate"), TEXT("EmitterUpdateScript")))
            {
                return EmitterData->EmitterUpdateScriptProps.Script;
            }
            if (ScriptUsageMatches(ScriptUsage, TEXT("ParticleSpawn"), TEXT("ParticleSpawnScript"))
                || ScriptUsage.Equals(TEXT("Spawn"), ESearchCase::IgnoreCase))
            {
                return EmitterData->SpawnScriptProps.Script;
            }
            return EmitterData->UpdateScriptProps.Script;
        }

        if (System)
        {
            if (ScriptUsageMatches(ScriptUsage, TEXT("SystemUpdate"), TEXT("Update"))
                || ScriptUsage.Equals(TEXT("Update"), ESearchCase::IgnoreCase))
            {
                return System->GetSystemUpdateScript();
            }
            return System->GetSystemSpawnScript();
        }

        return nullptr;
    }

    FNiagaraEmitterHandle* FindEmitterHandle(UNiagaraSystem* System, const FString& EmitterName)
    {
        if (!System)
        {
            return nullptr;
        }
        for (const FNiagaraEmitterHandle& Handle : System->GetEmitterHandles())
        {
            if (Handle.GetName().ToString().Equals(EmitterName, ESearchCase::IgnoreCase))
            {
                return const_cast<FNiagaraEmitterHandle*>(&Handle);
            }
        }
        return nullptr;
    }

    // The `ownerName` a resolved target's stack entries carry in the dump: the emitter handle
    // name for a system emitter, the system name for a system-scope stack, the emitter asset
    // name for a standalone emitter. Must stay in step with NiagaraDumpBuilder's stack owners,
    // because owner-qualified entry keys are composed there and checked here.
    FString ResolvedStackOwnerName(const FNiagaraResolvedTarget& Target)
    {
        if (Target.EmitterHandle)
        {
            return Target.EmitterHandle->GetName().ToString();
        }
        if (Target.System)
        {
            return Target.System->GetName();
        }
        return Target.Emitter ? Target.Emitter->GetName() : FString();
    }

    UEdGraphNode* FindNode(UNiagaraGraph* Graph, const FString& NodeId)
    {
        if (!Graph || NodeId.IsEmpty())
        {
            return nullptr;
        }
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (!Node)
            {
                continue;
            }
            if (Node->NodeGuid.ToString().Equals(NodeId, ESearchCase::IgnoreCase)
                || Node->GetName().Equals(NodeId, ESearchCase::IgnoreCase)
                || Node->GetNodeTitle(ENodeTitleType::ListView).ToString().Equals(NodeId, ESearchCase::IgnoreCase))
            {
                return Node;
            }
        }
        return nullptr;
    }

    UEdGraphPin* FindPin(UEdGraphNode* Node, const FString& PinName)
    {
        if (!Node || PinName.IsEmpty())
        {
            return nullptr;
        }
        for (UEdGraphPin* Pin : Node->Pins)
        {
            if (!Pin)
            {
                continue;
            }
            if (Pin->PinId.ToString().Equals(PinName, ESearchCase::IgnoreCase)
                || Pin->PinName.ToString().Equals(PinName, ESearchCase::IgnoreCase)
                || Pin->GetDisplayName().ToString().Equals(PinName, ESearchCase::IgnoreCase))
            {
                return Pin;
            }
        }
        return nullptr;
    }

    FNiagaraEditError ResolveParameterStore(
        const FNiagaraEditTargetSpec& TargetSpec,
        FNiagaraResolvedTarget& OutTarget)
    {
        const FString Scope = TargetSpec.Scope.IsEmpty() ? TEXT("user") : TargetSpec.Scope;

        // user, systemSpawnRapidIteration and systemUpdateRapidIteration all resolve to a store
        // hanging off the UNiagaraSystem, so a resolved emitter cannot change what is written.
        // Refuse here rather than in each verb: every caller of this resolver (set_parameter /
        // add_parameter / remove_parameter, set_curve_keys / bind_curve_asset, add_data_interface /
        // remove_data_interface) then shares one rule, and none of them reports success for a
        // write the named emitter had nothing to do with.
        const bool bSystemWideScope = Scope.Equals(TEXT("user"), ESearchCase::IgnoreCase)
            || Scope.Equals(TEXT("systemSpawnRapidIteration"), ESearchCase::IgnoreCase)
            || Scope.Equals(TEXT("systemUpdateRapidIteration"), ESearchCase::IgnoreCase);
        if (bSystemWideScope && !TargetSpec.EmitterName.IsEmpty())
        {
            return FNiagaraEditError::Make(
                TEXT("INVALID_ARGUMENT"),
                FString::Printf(
                    TEXT("Parameter scope '%s' is system-wide and does not read 'emitter' (got '%s'). Omit 'emitter', or pass an emitter scope (rendererBindings, spawnRapidIteration, updateRapidIteration) to write to that emitter."),
                    *Scope, *TargetSpec.EmitterName));
        }

        if (Scope.Equals(TEXT("user"), ESearchCase::IgnoreCase))
        {
            if (!OutTarget.System)
            {
                return FNiagaraEditError::Make(TEXT("UNSUPPORTED_SCOPE"), TEXT("User parameters require a Niagara System asset."));
            }
            OutTarget.ParameterStore = &OutTarget.System->GetExposedParameters();
            return FNiagaraEditError();
        }

        if (Scope.Equals(TEXT("systemSpawnRapidIteration"), ESearchCase::IgnoreCase))
        {
            UNiagaraScript* Script = OutTarget.System ? OutTarget.System->GetSystemSpawnScript() : nullptr;
            OutTarget.ParameterStore = Script ? &Script->RapidIterationParameters : nullptr;
            return OutTarget.ParameterStore ? FNiagaraEditError() : FNiagaraEditError::Make(TEXT("PARAMETER_STORE_NOT_FOUND"), TEXT("System spawn rapid-iteration store is not available."));
        }

        if (Scope.Equals(TEXT("systemUpdateRapidIteration"), ESearchCase::IgnoreCase))
        {
            UNiagaraScript* Script = OutTarget.System ? OutTarget.System->GetSystemUpdateScript() : nullptr;
            OutTarget.ParameterStore = Script ? &Script->RapidIterationParameters : nullptr;
            return OutTarget.ParameterStore ? FNiagaraEditError() : FNiagaraEditError::Make(TEXT("PARAMETER_STORE_NOT_FOUND"), TEXT("System update rapid-iteration store is not available."));
        }

        if (!OutTarget.EmitterData)
        {
            return FNiagaraEditError::Make(TEXT("EMITTER_REQUIRED"), FString::Printf(TEXT("Parameter scope '%s' requires an emitter."), *Scope));
        }

        if (Scope.Equals(TEXT("rendererBindings"), ESearchCase::IgnoreCase))
        {
            OutTarget.ParameterStore = &OutTarget.EmitterData->RendererBindings;
        }
        else if (Scope.Equals(TEXT("spawnRapidIteration"), ESearchCase::IgnoreCase))
        {
            UNiagaraScript* Script = OutTarget.EmitterData->SpawnScriptProps.Script;
            OutTarget.ParameterStore = Script ? &Script->RapidIterationParameters : nullptr;
        }
        else if (Scope.Equals(TEXT("updateRapidIteration"), ESearchCase::IgnoreCase))
        {
            UNiagaraScript* Script = OutTarget.EmitterData->UpdateScriptProps.Script;
            OutTarget.ParameterStore = Script ? &Script->RapidIterationParameters : nullptr;
        }
        else
        {
            return FNiagaraEditError::Make(TEXT("UNSUPPORTED_SCOPE"), FString::Printf(TEXT("Unsupported Niagara parameter scope '%s'."), *Scope));
        }

        return OutTarget.ParameterStore ? FNiagaraEditError() : FNiagaraEditError::Make(TEXT("PARAMETER_STORE_NOT_FOUND"), FString::Printf(TEXT("Parameter store for scope '%s' is not available."), *Scope));
    }

    FNiagaraEditError ResolveGraphTarget(
        const FNiagaraEditTargetSpec& TargetSpec,
        FNiagaraResolvedTarget& OutTarget)
    {
        UNiagaraScript* Script = ResolveScript(OutTarget.System, OutTarget.EmitterData, TargetSpec.ScriptUsage);
        OutTarget.Graph = NiagaraJsonHelpers::GetGraphFromScript(Script);
        if (!OutTarget.Graph)
        {
            return FNiagaraEditError::Make(TEXT("GRAPH_NOT_FOUND"), TEXT("Could not resolve target Niagara graph."));
        }
        OutTarget.ReflectedObject = OutTarget.Graph;
        return FNiagaraEditError();
    }

    bool HasSystemCompileSources(const UNiagaraSystem& System)
    {
        const UNiagaraScript* SpawnScript = System.GetSystemSpawnScript();
        const UNiagaraScript* UpdateScript = System.GetSystemUpdateScript();
        return SpawnScript
            && SpawnScript->GetLatestSource() != nullptr
            && UpdateScript
            && UpdateScript->GetLatestSource() != nullptr;
    }
}

namespace NiagaraEdit
{
    bool TryParseStackScriptUsageAlias(const FString& ScriptUsage, ENiagaraScriptUsage& OutUsage)
    {
        FString Normalized = ScriptUsage.TrimStartAndEnd();
        const FString EnumPrefix(TEXT("ENiagaraScriptUsage::"));
        if (Normalized.StartsWith(EnumPrefix, ESearchCase::IgnoreCase))
        {
            Normalized = Normalized.Mid(EnumPrefix.Len()).TrimStartAndEnd();
        }
        if (Normalized.EndsWith(TEXT("Script"), ESearchCase::IgnoreCase))
        {
            Normalized = Normalized.Left(Normalized.Len() - 6).TrimStartAndEnd();
        }

        struct FStackScriptUsageAlias
        {
            const TCHAR* Alias;
            ENiagaraScriptUsage Usage;
        };

        static const FStackScriptUsageAlias UsageAliases[] = {
            { TEXT("ParticleSpawn"), ENiagaraScriptUsage::ParticleSpawnScript },
            { TEXT("ParticleUpdate"), ENiagaraScriptUsage::ParticleUpdateScript },
            { TEXT("EmitterSpawn"), ENiagaraScriptUsage::EmitterSpawnScript },
            { TEXT("EmitterUpdate"), ENiagaraScriptUsage::EmitterUpdateScript },
            { TEXT("SystemSpawn"), ENiagaraScriptUsage::SystemSpawnScript },
            { TEXT("SystemUpdate"), ENiagaraScriptUsage::SystemUpdateScript },
        };

        for (const FStackScriptUsageAlias& Entry : UsageAliases)
        {
            if (Normalized.Equals(Entry.Alias, ESearchCase::IgnoreCase))
            {
                OutUsage = Entry.Usage;
                return true;
            }
        }
        return false;
    }

    FString StackScriptUsageToString(ENiagaraScriptUsage Usage)
    {
        switch (Usage)
        {
        case ENiagaraScriptUsage::ParticleSpawnScript:
            return TEXT("ParticleSpawn");
        case ENiagaraScriptUsage::ParticleUpdateScript:
            return TEXT("ParticleUpdate");
        case ENiagaraScriptUsage::EmitterSpawnScript:
            return TEXT("EmitterSpawn");
        case ENiagaraScriptUsage::EmitterUpdateScript:
            return TEXT("EmitterUpdate");
        case ENiagaraScriptUsage::SystemSpawnScript:
            return TEXT("SystemSpawn");
        case ENiagaraScriptUsage::SystemUpdateScript:
            return TEXT("SystemUpdate");
        default:
            break;
        }

        const UEnum* UsageEnum = StaticEnum<ENiagaraScriptUsage>();
        FString UsageName = UsageEnum
            ? UsageEnum->GetNameStringByValue(static_cast<int64>(Usage))
            : FString();
        const FString EnumPrefix(TEXT("ENiagaraScriptUsage::"));
        if (UsageName.StartsWith(EnumPrefix, ESearchCase::IgnoreCase))
        {
            UsageName = UsageName.Mid(EnumPrefix.Len());
        }
        if (UsageName.EndsWith(TEXT("Script"), ESearchCase::IgnoreCase))
        {
            UsageName = UsageName.Left(UsageName.Len() - 6);
        }
        return UsageName.IsEmpty() ? TEXT("Unknown") : UsageName;
    }

    FString TargetKindToString(ENiagaraEditTargetKind Kind)
    {
        switch (Kind)
        {
        case ENiagaraEditTargetKind::System: return TEXT("system");
        case ENiagaraEditTargetKind::EmitterHandle: return TEXT("emitterHandle");
        case ENiagaraEditTargetKind::EmitterData: return TEXT("emitterData");
        case ENiagaraEditTargetKind::Renderer: return TEXT("renderer");
        case ENiagaraEditTargetKind::ParameterStore: return TEXT("parameterStore");
        case ENiagaraEditTargetKind::Graph: return TEXT("graph");
        case ENiagaraEditTargetKind::Node: return TEXT("node");
        case ENiagaraEditTargetKind::Pin: return TEXT("pin");
        case ENiagaraEditTargetKind::DataInterface: return TEXT("dataInterface");
        case ENiagaraEditTargetKind::Module: return TEXT("module");
        case ENiagaraEditTargetKind::EventHandler: return TEXT("eventHandler");
        case ENiagaraEditTargetKind::SimulationStage: return TEXT("simulationStage");
        default: return TEXT("unknown");
        }
    }

    FNiagaraEditError RejectBatchPayload(const TSharedPtr<FJsonObject>& Payload)
    {
        if (HasField(Payload, TEXT("operations")))
        {
            return FNiagaraEditError::Make(TEXT("INVALID_ARGUMENT"), TEXT("Niagara edit RPCs accept exactly one operation; 'operations' arrays are not supported."));
        }
        return FNiagaraEditError();
    }

    FNiagaraEditError ParsePropertyPayload(const TSharedPtr<FJsonObject>& Payload, FNiagaraPropertyEditPayload& OutPayload)
    {
        if (FNiagaraEditError Error = RejectBatchPayload(Payload); Error.HasError())
        {
            return Error;
        }
        OutPayload.AssetPath = GetStringField(Payload, TEXT("assetPath"));
        if (OutPayload.AssetPath.IsEmpty())
        {
            return FNiagaraEditError::Make(TEXT("INVALID_ARGUMENT"), TEXT("Missing 'assetPath'."));
        }
        if (FNiagaraEditError Error = ParseTargetSpec(Payload, ENiagaraEditTargetKind::Unknown, OutPayload.Target); Error.HasError())
        {
            return Error;
        }
        if (OutPayload.Target.Kind == ENiagaraEditTargetKind::Unknown)
        {
            return FNiagaraEditError::Make(TEXT("INVALID_ARGUMENT"), TEXT("Missing target.kind."));
        }
        OutPayload.PropertyPath = GetStringField(Payload, TEXT("propertyPath"));
        if (OutPayload.PropertyPath.IsEmpty())
        {
            return FNiagaraEditError::Make(TEXT("INVALID_ARGUMENT"), TEXT("Missing 'propertyPath'."));
        }
        OutPayload.Value = GetFieldValue(Payload, TEXT("value"));
        if (!OutPayload.Value.IsValid())
        {
            return FNiagaraEditError::Make(TEXT("INVALID_ARGUMENT"), TEXT("Missing 'value'."));
        }
        OutPayload.Options = ParseOptions(Payload);
        return FNiagaraEditError();
    }

    FNiagaraEditError ParseParameterPayload(const TSharedPtr<FJsonObject>& Payload, ENiagaraEditOperation Operation, FNiagaraParameterEditPayload& OutPayload)
    {
        if (FNiagaraEditError Error = RejectBatchPayload(Payload); Error.HasError())
        {
            return Error;
        }
        OutPayload.AssetPath = GetStringField(Payload, TEXT("assetPath"));
        OutPayload.Scope = GetStringField(Payload, TEXT("scope"));
        OutPayload.EmitterName = GetStringField(Payload, TEXT("emitter"));
        if (OutPayload.EmitterName.IsEmpty())
        {
            OutPayload.EmitterName = GetStringField(Payload, TEXT("emitterName"));
        }
        OutPayload.Name = GetStringField(Payload, TEXT("name"));
        if (OutPayload.Name.IsEmpty())
        {
            OutPayload.Name = GetStringField(Payload, TEXT("parameterName"));
        }
        OutPayload.Type = GetStringField(Payload, TEXT("type"));
        if (OutPayload.Type.IsEmpty())
        {
            OutPayload.Type = GetStringField(Payload, TEXT("parameterType"));
        }
        // A non-string `type` must not read as an absent one. niagara.inspect reports a
        // parameter's type as an OBJECT ({name, struct, ...}), and feeding that entry straight
        // back is the natural way to round-trip a store; GetStringField answers "" for it, so
        // without this the refusal would say the field was never sent.
        const TCHAR* MisTypedTypeKey = nullptr;
        if (OutPayload.Type.IsEmpty())
        {
            for (const TCHAR* TypeKey : {TEXT("type"), TEXT("parameterType")})
            {
                const TSharedPtr<FJsonValue> RawType = GetFieldValue(Payload, TypeKey);
                if (RawType.IsValid() && RawType->Type != EJson::Null && RawType->Type != EJson::None)
                {
                    MisTypedTypeKey = TypeKey;
                    break;
                }
            }
        }
        OutPayload.Value = Operation == ENiagaraEditOperation::AddParameter
            ? GetFieldValue(Payload, TEXT("defaultValue"))
            : GetFieldValue(Payload, TEXT("value"));
        if (Operation == ENiagaraEditOperation::AddParameter && !OutPayload.Value.IsValid())
        {
            OutPayload.Value = GetFieldValue(Payload, TEXT("value"));
        }
        OutPayload.Options = ParseOptions(Payload);

        if (OutPayload.AssetPath.IsEmpty())
        {
            return FNiagaraEditError::Make(TEXT("INVALID_ARGUMENT"), TEXT("Missing 'assetPath'."));
        }
        if (OutPayload.Scope.IsEmpty())
        {
            return FNiagaraEditError::Make(TEXT("INVALID_ARGUMENT"), TEXT("Missing 'scope'."));
        }
        if (OutPayload.Name.IsEmpty())
        {
            return FNiagaraEditError::Make(TEXT("INVALID_ARGUMENT"), TEXT("Missing parameter 'name'."));
        }
        if (Operation != ENiagaraEditOperation::RemoveParameter && OutPayload.Type.IsEmpty())
        {
            if (MisTypedTypeKey)
            {
                return FNiagaraEditError::Make(
                    TEXT("INVALID_ARGUMENT"),
                    FString::Printf(
                        TEXT("Parameter '%s' must be a string naming the type (for example \"int32\" or \"NiagaraInt32\"). ")
                        TEXT("niagara.inspect reports a parameter's type as an object; send its 'name' field, not the object."),
                        MisTypedTypeKey));
            }
            return FNiagaraEditError::Make(TEXT("INVALID_ARGUMENT"), TEXT("Missing parameter 'type'."));
        }
        if (Operation != ENiagaraEditOperation::RemoveParameter)
        {
            return ValidateTypedValue(OutPayload.Type, OutPayload.Value, Operation == ENiagaraEditOperation::AddParameter ? TEXT("defaultValue") : TEXT("value"));
        }
        return FNiagaraEditError();
    }

    FNiagaraEditError ParseRendererPayload(const TSharedPtr<FJsonObject>& Payload, ENiagaraEditOperation Operation, FNiagaraRendererEditPayload& OutPayload)
    {
        if (FNiagaraEditError Error = RejectBatchPayload(Payload); Error.HasError())
        {
            return Error;
        }
        const ENiagaraEditTargetKind DefaultKind = Operation == ENiagaraEditOperation::AddRenderer
            ? ENiagaraEditTargetKind::EmitterData
            : ENiagaraEditTargetKind::Renderer;
        OutPayload.AssetPath = GetStringField(Payload, TEXT("assetPath"));
        if (OutPayload.AssetPath.IsEmpty())
        {
            return FNiagaraEditError::Make(TEXT("INVALID_ARGUMENT"), TEXT("Missing 'assetPath'."));
        }
        if (FNiagaraEditError Error = ParseTargetSpec(Payload, DefaultKind, OutPayload.Target); Error.HasError())
        {
            return Error;
        }
        OutPayload.RendererClassPath = GetStringField(Payload, TEXT("rendererClassPath"));
        if (OutPayload.RendererClassPath.IsEmpty())
        {
            OutPayload.RendererClassPath = GetStringField(Payload, TEXT("classPath"));
        }
        OutPayload.Options = ParseOptions(Payload);

        if (Operation == ENiagaraEditOperation::AddRenderer && OutPayload.RendererClassPath.IsEmpty())
        {
            return FNiagaraEditError::Make(TEXT("INVALID_ARGUMENT"), TEXT("Missing 'rendererClassPath'."));
        }
        return FNiagaraEditError();
    }

    // The ordinal half of the renderer family. niagara.remove_renderer and niagara.move_renderer
    // declare `index` REQUIRED and nothing else feeds FNiagaraEditTargetSpec::Index for them, while
    // niagara.add_renderer appends (UNiagaraEmitter::AddRenderer) and never reads Target.Index at
    // all. The read therefore lives in a function only the two ordinal verbs call, rather than in
    // ParseRendererPayload -- which add_renderer also calls, and which would hand it a key its
    // RPC_PARAMS refuses (PinWright.infra.declared_params.HandlersOnlyReadDeclaredParams; the
    // guard is flow-INSENSITIVE, so an `if (Operation != AddRenderer)` around the read would not
    // have removed the pair -- only moving it to a different function does).
    // Order is load-bearing: the flat read runs AFTER ParseRendererPayload's ParseTargetSpec, so a
    // top-level `index` still wins over a nested `target.index`, exactly as before the split.
    FNiagaraEditError ParseRendererOrdinalPayload(const TSharedPtr<FJsonObject>& Payload, ENiagaraEditOperation Operation, FNiagaraRendererEditPayload& OutPayload)
    {
        if (FNiagaraEditError Error = ParseRendererPayload(Payload, Operation, OutPayload); Error.HasError())
        {
            return Error;
        }
        TryGetIntField(Payload, TEXT("index"), OutPayload.Target.Index);
        if (OutPayload.Target.Index == INDEX_NONE)
        {
            return FNiagaraEditError::Make(TEXT("INVALID_ARGUMENT"), TEXT("Missing renderer 'index'."));
        }
        if (Operation == ENiagaraEditOperation::MoveRenderer && OutPayload.Target.ToIndex == INDEX_NONE)
        {
            return FNiagaraEditError::Make(TEXT("INVALID_ARGUMENT"), TEXT("Missing renderer 'toIndex'."));
        }
        return FNiagaraEditError();
    }

    FNiagaraEditError ParseModulePayload(const TSharedPtr<FJsonObject>& Payload, ENiagaraEditOperation Operation, FNiagaraModuleEditPayload& OutPayload)
    {
        if (FNiagaraEditError Error = RejectBatchPayload(Payload); Error.HasError())
        {
            return Error;
        }
        const ENiagaraEditTargetKind DefaultKind = Operation == ENiagaraEditOperation::AddModule
            ? ENiagaraEditTargetKind::Graph
            : ENiagaraEditTargetKind::Module;
        OutPayload.AssetPath = GetStringField(Payload, TEXT("assetPath"));
        if (OutPayload.AssetPath.IsEmpty())
        {
            return FNiagaraEditError::Make(TEXT("INVALID_ARGUMENT"), TEXT("Missing 'assetPath'."));
        }
        if (FNiagaraEditError Error = ParseTargetSpec(Payload, DefaultKind, OutPayload.Target); Error.HasError())
        {
            return Error;
        }
        OutPayload.ModulePath = GetStringField(Payload, TEXT("modulePath"));
        OutPayload.InputName = GetStringField(Payload, TEXT("inputName"));
        OutPayload.Value = GetFieldValue(Payload, TEXT("value"));
        OutPayload.Options = ParseOptions(Payload);

        if (Operation == ENiagaraEditOperation::SetModuleScript)
        {
            OutPayload.NewScriptPath = GetStringField(Payload, TEXT("scriptPath"));
            const FString VersionStr = GetStringField(Payload, TEXT("scriptVersion"));
            if (!VersionStr.IsEmpty())
            {
                FGuid::Parse(VersionStr, OutPayload.NewScriptVersion);
            }
            // bPreserveOverrides defaults to true; only override if the field is present and false
            OutPayload.bPreserveOverrides = true;
            if (Payload.IsValid() && Payload->HasField(TEXT("preserveOverrides")))
            {
                Payload->TryGetBoolField(TEXT("preserveOverrides"), OutPayload.bPreserveOverrides);
            }
        }

        if (Operation == ENiagaraEditOperation::AddModule && OutPayload.ModulePath.IsEmpty())
        {
            return FNiagaraEditError::Make(TEXT("INVALID_ARGUMENT"), TEXT("Missing 'modulePath'."));
        }
        if ((Operation == ENiagaraEditOperation::RemoveModule
            || Operation == ENiagaraEditOperation::MoveModule
            || Operation == ENiagaraEditOperation::SetStackEnabled
            || Operation == ENiagaraEditOperation::SetModuleInput
            || Operation == ENiagaraEditOperation::SetModuleScript
            || Operation == ENiagaraEditOperation::ResetModuleInput
            || Operation == ENiagaraEditOperation::ClearModuleOverrides)
            && OutPayload.Target.EntryId.IsEmpty())
        {
            return FNiagaraEditError::Make(TEXT("INVALID_ARGUMENT"), TEXT("Missing module 'entryId'."));
        }
        if (Operation == ENiagaraEditOperation::MoveModule && OutPayload.Target.ToIndex == INDEX_NONE)
        {
            return FNiagaraEditError::Make(TEXT("INVALID_ARGUMENT"), TEXT("Missing module 'toIndex'."));
        }
        if (Operation == ENiagaraEditOperation::SetModuleInput && OutPayload.InputName.IsEmpty())
        {
            return FNiagaraEditError::Make(TEXT("INVALID_ARGUMENT"), TEXT("Missing 'inputName'."));
        }
        if (Operation == ENiagaraEditOperation::SetModuleInput && !OutPayload.Value.IsValid())
        {
            return FNiagaraEditError::Make(TEXT("INVALID_ARGUMENT"), TEXT("Missing 'value'."));
        }
        if (Operation == ENiagaraEditOperation::ResetModuleInput && OutPayload.InputName.IsEmpty())
        {
            return FNiagaraEditError::Make(TEXT("INVALID_ARGUMENT"), TEXT("Missing 'inputName'."));
        }
        if (Operation == ENiagaraEditOperation::SetStackEnabled && !HasField(Payload, TEXT("enabled")))
        {
            return FNiagaraEditError::Make(TEXT("INVALID_ARGUMENT"), TEXT("Missing 'enabled'."));
        }
        if (Operation == ENiagaraEditOperation::SetStackEnabled)
        {
            bool bEnabled = false;
            if (!Payload.IsValid() || !Payload->TryGetBoolField(TEXT("enabled"), bEnabled))
            {
                return FNiagaraEditError::Make(TEXT("INVALID_VALUE"), TEXT("'enabled' must be a boolean."));
            }
        }
        if (Operation == ENiagaraEditOperation::SetModuleScript && OutPayload.NewScriptPath.IsEmpty())
        {
            return FNiagaraEditError::Make(TEXT("INVALID_ARGUMENT"), TEXT("Missing 'scriptPath'."));
        }
        return FNiagaraEditError();
    }

    FNiagaraEditError ParsePinPayload(const TSharedPtr<FJsonObject>& Payload, ENiagaraEditOperation Operation, FNiagaraPinEditPayload& OutPayload)
    {
        if (FNiagaraEditError Error = RejectBatchPayload(Payload); Error.HasError())
        {
            return Error;
        }
        OutPayload.AssetPath = GetStringField(Payload, TEXT("assetPath"));
        if (OutPayload.AssetPath.IsEmpty())
        {
            return FNiagaraEditError::Make(TEXT("INVALID_ARGUMENT"), TEXT("Missing 'assetPath'."));
        }
        if (FNiagaraEditError Error = ParseTargetSpec(Payload, Operation == ENiagaraEditOperation::SetPinDefault ? ENiagaraEditTargetKind::Pin : ENiagaraEditTargetKind::Graph, OutPayload.Target); Error.HasError())
        {
            return Error;
        }
        OutPayload.FromNode = GetStringField(Payload, TEXT("fromNode"));
        OutPayload.FromPin = GetStringField(Payload, TEXT("fromPin"));
        OutPayload.ToNode = GetStringField(Payload, TEXT("toNode"));
        OutPayload.ToPin = GetStringField(Payload, TEXT("toPin"));
        OutPayload.NodeId = GetStringField(Payload, TEXT("nodeId"));
        if (OutPayload.NodeId.IsEmpty())
        {
            OutPayload.NodeId = OutPayload.Target.NodeId;
        }
        OutPayload.PinName = GetStringField(Payload, TEXT("pin"));
        if (OutPayload.PinName.IsEmpty())
        {
            OutPayload.PinName = GetStringField(Payload, TEXT("pinName"));
        }
        if (OutPayload.PinName.IsEmpty())
        {
            OutPayload.PinName = OutPayload.Target.PinName;
        }
        OutPayload.DefaultValue = GetFieldValue(Payload, TEXT("defaultValue"));
        if (!OutPayload.DefaultValue.IsValid())
        {
            OutPayload.DefaultValue = GetFieldValue(Payload, TEXT("value"));
        }
        OutPayload.Options = ParseOptions(Payload);

        if (Operation == ENiagaraEditOperation::SetPinDefault)
        {
            if (OutPayload.NodeId.IsEmpty())
            {
                return FNiagaraEditError::Make(TEXT("INVALID_ARGUMENT"), TEXT("Missing 'nodeId'."));
            }
            if (OutPayload.PinName.IsEmpty())
            {
                return FNiagaraEditError::Make(TEXT("INVALID_ARGUMENT"), TEXT("Missing 'pin'."));
            }
            if (!OutPayload.DefaultValue.IsValid())
            {
                return FNiagaraEditError::Make(TEXT("INVALID_ARGUMENT"), TEXT("Missing 'defaultValue'."));
            }
            OutPayload.Target.NodeId = OutPayload.NodeId;
            OutPayload.Target.PinName = OutPayload.PinName;
            return FNiagaraEditError();
        }

        if (OutPayload.FromNode.IsEmpty() || OutPayload.FromPin.IsEmpty() || OutPayload.ToNode.IsEmpty() || OutPayload.ToPin.IsEmpty())
        {
            return FNiagaraEditError::Make(TEXT("INVALID_ARGUMENT"), TEXT("connect/disconnect pin requires fromNode, fromPin, toNode, and toPin."));
        }
        return FNiagaraEditError();
    }

    FString MakeModuleEntryKey(const FString& OwnerName, const FGuid& NodeGuid)
    {
        return FString::Printf(TEXT("%s:%s"), *OwnerName, *NodeGuid.ToString());
    }

    bool TrySplitModuleEntryKey(const FString& EntryKey, FString& OutOwnerName, FString& OutEntryId)
    {
        int32 SeparatorIndex = INDEX_NONE;
        if (!EntryKey.FindLastChar(TEXT(':'), SeparatorIndex) || SeparatorIndex == 0)
        {
            return false;
        }
        const FString Candidate = EntryKey.Mid(SeparatorIndex + 1);
        // Only a Guid tail makes this a qualified key. Module ids also resolve by node name and
        // node title (see FindNode), and a title may legitimately contain a colon.
        FGuid ParsedGuid;
        if (!FGuid::Parse(Candidate, ParsedGuid))
        {
            return false;
        }
        OutOwnerName = EntryKey.Left(SeparatorIndex);
        OutEntryId = Candidate;
        return true;
    }

    FNiagaraEditError ResolveTarget(const FString& AssetPath, const FNiagaraEditTargetSpec& TargetSpec, FNiagaraResolvedTarget& OutTarget)
    {
        OutTarget = FNiagaraResolvedTarget();
        OutTarget.AssetPath = AssetPath;
        OutTarget.Asset = LoadObject<UObject>(nullptr, *AssetPath);
        if (!OutTarget.Asset)
        {
            return FNiagaraEditError::Make(TEXT("ASSET_NOT_FOUND"), FString::Printf(TEXT("Could not load Niagara asset '%s'."), *AssetPath));
        }

        OutTarget.System = Cast<UNiagaraSystem>(OutTarget.Asset);
        OutTarget.Emitter = Cast<UNiagaraEmitter>(OutTarget.Asset);
        if (!OutTarget.System && !OutTarget.Emitter)
        {
            return FNiagaraEditError::Make(TEXT("UNSUPPORTED_ASSET"), TEXT("Asset is not a Niagara System or Niagara Emitter."));
        }
        OutTarget.AssetKind = OutTarget.System ? TEXT("NiagaraSystem") : TEXT("NiagaraEmitter");

        // Asset-kind guard. An open emitter toolkit edits a DUPLICATE of the emitter asset and
        // re-duplicates that copy over the original on Apply, so a write landing on the original
        // here is invisible to the toolkit and destroyed by the next Apply after reporting
        // success. Every ResolveTarget caller is a mutator, and the hazard is a property of the
        // asset rather than of any one verb, so it is refused once here instead of being adopted
        // per verb. Systems are unaffected — their toolkit edits the asset itself. Full engine
        // call chain in NiagaraEditorOpenGuard.h.
        if (OutTarget.Emitter && !OutTarget.System)
        {
            FString RefusalMessage;
            if (PinWrightNiagara::RefuseEmitterAssetEditWhileToolkitOpen(
                    OutTarget.Asset, AssetPath, RefusalMessage))
            {
                return FNiagaraEditError::Make(TEXT("EDITOR_OPEN"), RefusalMessage);
            }
        }

        // An owner-qualified `entryId` names the emitter that scopes it, so it can stand in for
        // an omitted `emitter`: the qualified form is then a self-sufficient addressing key that
        // a caller can store and replay without carrying the owner in a second field. Only
        // adopted when the owner actually names an emitter handle — a system-scope entry key is
        // qualified with the system's own name and must not be looked up as an emitter.
        FString EffectiveEmitterName = TargetSpec.EmitterName;
        if (EffectiveEmitterName.IsEmpty()
            && !TargetSpec.EntryOwner.IsEmpty()
            && OutTarget.System
            && FindEmitterHandle(OutTarget.System, TargetSpec.EntryOwner))
        {
            EffectiveEmitterName = TargetSpec.EntryOwner;
        }

        const bool bNeedsEmitter = TargetSpec.Kind == ENiagaraEditTargetKind::EmitterHandle
            || TargetSpec.Kind == ENiagaraEditTargetKind::EmitterData
            || TargetSpec.Kind == ENiagaraEditTargetKind::Renderer
            || TargetSpec.Kind == ENiagaraEditTargetKind::DataInterface
            || TargetSpec.Kind == ENiagaraEditTargetKind::EventHandler
            || TargetSpec.Kind == ENiagaraEditTargetKind::SimulationStage
            || (TargetSpec.Kind == ENiagaraEditTargetKind::Graph && !EffectiveEmitterName.IsEmpty())
            || TargetSpec.Kind == ENiagaraEditTargetKind::Node
            || TargetSpec.Kind == ENiagaraEditTargetKind::Pin
            || (TargetSpec.Kind == ENiagaraEditTargetKind::Module
                && ScriptUsageRequiresEmitter(TargetSpec.ScriptUsage));

        if (OutTarget.System && (bNeedsEmitter || !EffectiveEmitterName.IsEmpty()))
        {
            if (EffectiveEmitterName.IsEmpty()
                && TargetSpec.Kind != ENiagaraEditTargetKind::Graph
                && TargetSpec.Kind != ENiagaraEditTargetKind::Node
                && TargetSpec.Kind != ENiagaraEditTargetKind::Pin)
            {
                return FNiagaraEditError::Make(TEXT("EMITTER_REQUIRED"), TEXT("Target requires 'emitter'."));
            }
            if (!EffectiveEmitterName.IsEmpty())
            {
                OutTarget.EmitterHandle = FindEmitterHandle(OutTarget.System, EffectiveEmitterName);
                if (!OutTarget.EmitterHandle)
                {
                    return FNiagaraEditError::Make(TEXT("EMITTER_NOT_FOUND"), FString::Printf(TEXT("Emitter '%s' was not found."), *EffectiveEmitterName));
                }
                const FVersionedNiagaraEmitter Instance = OutTarget.EmitterHandle->GetInstance();
                OutTarget.Emitter = Instance.Emitter;
                OutTarget.EmitterData = OutTarget.EmitterHandle->GetEmitterData();
            }
        }
        else if (OutTarget.Emitter)
        {
            OutTarget.EmitterData = OutTarget.Emitter->GetLatestEmitterData();
        }

        switch (TargetSpec.Kind)
        {
        case ENiagaraEditTargetKind::System:
            if (!OutTarget.System)
            {
                return FNiagaraEditError::Make(TEXT("INVALID_TARGET"), TEXT("Target kind 'system' requires a Niagara System asset."));
            }
            OutTarget.ReflectedObject = OutTarget.System;
            return FNiagaraEditError();

        case ENiagaraEditTargetKind::EmitterHandle:
            if (!OutTarget.EmitterHandle)
            {
                return FNiagaraEditError::Make(TEXT("INVALID_TARGET"), TEXT("Target kind 'emitterHandle' requires a system emitter handle."));
            }
            // FNiagaraEmitterHandle is a USTRUCT() with reflected UPROPERTY() fields (e.g. bIsEnabled);
            // wire the handle struct so those handle-level properties are settable through this kind.
            // Emitter-data fields like bLocalSpace live on FVersionedNiagaraEmitterData and are reached
            // via the EmitterData case below — the two scopes are distinct, so do NOT alias one to the other.
            OutTarget.ReflectedStruct = FNiagaraEmitterHandle::StaticStruct();
            OutTarget.ReflectedContainer = OutTarget.EmitterHandle;
            return FNiagaraEditError();

        case ENiagaraEditTargetKind::EmitterData:
            if (!OutTarget.EmitterData)
            {
                return FNiagaraEditError::Make(TEXT("EMITTER_DATA_MISSING"), TEXT("Versioned emitter data is not available."));
            }
            OutTarget.ReflectedStruct = FVersionedNiagaraEmitterData::StaticStruct();
            OutTarget.ReflectedContainer = OutTarget.EmitterData;
            return FNiagaraEditError();

        case ENiagaraEditTargetKind::Renderer:
            if (!OutTarget.EmitterData)
            {
                return FNiagaraEditError::Make(TEXT("EMITTER_DATA_MISSING"), TEXT("Renderer target requires versioned emitter data."));
            }
            if (TargetSpec.Index < 0 || TargetSpec.Index >= OutTarget.EmitterData->GetRenderers().Num())
            {
                return FNiagaraEditError::Make(TEXT("RENDERER_NOT_FOUND"), FString::Printf(TEXT("Renderer index %d is out of range."), TargetSpec.Index));
            }
            OutTarget.Renderer = OutTarget.EmitterData->GetRenderers()[TargetSpec.Index];
            OutTarget.ReflectedObject = OutTarget.Renderer;
            return OutTarget.Renderer ? FNiagaraEditError() : FNiagaraEditError::Make(TEXT("RENDERER_NOT_FOUND"), FString::Printf(TEXT("Renderer index %d is null."), TargetSpec.Index));

        case ENiagaraEditTargetKind::ParameterStore:
            return ResolveParameterStore(TargetSpec, OutTarget);

        case ENiagaraEditTargetKind::Graph:
            return ResolveGraphTarget(TargetSpec, OutTarget);

        case ENiagaraEditTargetKind::Node:
            if (FNiagaraEditError Error = ResolveGraphTarget(TargetSpec, OutTarget); Error.HasError())
            {
                return Error;
            }
            OutTarget.Node = FindNode(OutTarget.Graph, TargetSpec.NodeId);
            OutTarget.ReflectedObject = OutTarget.Node;
            return OutTarget.Node ? FNiagaraEditError() : FNiagaraEditError::Make(TEXT("NODE_NOT_FOUND"), FString::Printf(TEXT("Node '%s' was not found."), *TargetSpec.NodeId));

        case ENiagaraEditTargetKind::Pin:
            if (FNiagaraEditError Error = ResolveGraphTarget(TargetSpec, OutTarget); Error.HasError())
            {
                return Error;
            }
            OutTarget.Node = FindNode(OutTarget.Graph, TargetSpec.NodeId);
            if (!OutTarget.Node)
            {
                return FNiagaraEditError::Make(TEXT("NODE_NOT_FOUND"), FString::Printf(TEXT("Node '%s' was not found."), *TargetSpec.NodeId));
            }
            OutTarget.Pin = FindPin(OutTarget.Node, TargetSpec.PinName);
            return OutTarget.Pin ? FNiagaraEditError() : FNiagaraEditError::Make(TEXT("PIN_NOT_FOUND"), FString::Printf(TEXT("Pin '%s' was not found."), *TargetSpec.PinName));

        case ENiagaraEditTargetKind::DataInterface:
            if (FNiagaraEditError Error = ResolveParameterStore(TargetSpec, OutTarget); Error.HasError())
            {
                return Error;
            }
            if (!OutTarget.ParameterStore || TargetSpec.Index < 0 || TargetSpec.Index >= OutTarget.ParameterStore->GetDataInterfaces().Num())
            {
                return FNiagaraEditError::Make(TEXT("DATA_INTERFACE_NOT_FOUND"), FString::Printf(TEXT("Data interface index %d is out of range."), TargetSpec.Index));
            }
            OutTarget.DataInterface = OutTarget.ParameterStore->GetDataInterfaces()[TargetSpec.Index];
            OutTarget.ReflectedObject = OutTarget.DataInterface;
            return OutTarget.DataInterface ? FNiagaraEditError() : FNiagaraEditError::Make(TEXT("DATA_INTERFACE_NOT_FOUND"), FString::Printf(TEXT("Data interface index %d is null."), TargetSpec.Index));

        case ENiagaraEditTargetKind::Module:
            if (FNiagaraEditError Error = ResolveGraphTarget(TargetSpec, OutTarget); Error.HasError())
            {
                return Error;
            }
            // An owner-qualified id knows which emitter it came from, so a caller who pairs it
            // with a different `emitter` is refused here. Without this the bare id would resolve
            // against whichever emitter was named and silently edit that emitter's module, since
            // duplicated emitters carry byte-identical NodeGuids
            // (B-niagara-entry-id-not-unique-across-emitters).
            if (!TargetSpec.EntryOwner.IsEmpty()
                && !TargetSpec.EntryOwner.Equals(ResolvedStackOwnerName(OutTarget), ESearchCase::IgnoreCase))
            {
                return FNiagaraEditError::Make(
                    TEXT("MODULE_OWNER_MISMATCH"),
                    FString::Printf(
                        TEXT("Module entry key names owner '%s' but the target resolved to '%s'. ")
                        TEXT("`entryId` is a node guid that is unique only within one emitter, so the two must agree."),
                        *TargetSpec.EntryOwner,
                        *ResolvedStackOwnerName(OutTarget)));
            }
            OutTarget.Node = FindNode(OutTarget.Graph, TargetSpec.EntryId);
            OutTarget.ModuleNode = Cast<UNiagaraNodeFunctionCall>(OutTarget.Node);
            OutTarget.ReflectedObject = OutTarget.ModuleNode;
            return OutTarget.ModuleNode ? FNiagaraEditError() : FNiagaraEditError::Make(TEXT("MODULE_NOT_FOUND"), FString::Printf(TEXT("Module entry '%s' was not found."), *TargetSpec.EntryId));

        case ENiagaraEditTargetKind::EventHandler:
        {
            if (!OutTarget.EmitterData)
            {
                return FNiagaraEditError::Make(TEXT("EMITTER_DATA_MISSING"), TEXT("Event handler target requires versioned emitter data."));
            }
            TArray<FNiagaraEventScriptProperties>& Handlers = OutTarget.EmitterData->EventHandlerScriptProps;
            int32 ResolvedIndex = INDEX_NONE;
            if (!TargetSpec.EntryId.IsEmpty())
            {
                FGuid UsageId;
                if (!FGuid::Parse(TargetSpec.EntryId, UsageId))
                {
                    return FNiagaraEditError::Make(TEXT("EVENT_HANDLER_INVALID_INDEX"), FString::Printf(TEXT("Event handler entryId '%s' is not a valid Guid."), *TargetSpec.EntryId));
                }
                ResolvedIndex = Handlers.IndexOfByPredicate([&UsageId](const FNiagaraEventScriptProperties& Props)
                {
                    return Props.Script != nullptr && Props.Script->GetUsageId() == UsageId;
                });
                if (ResolvedIndex == INDEX_NONE)
                {
                    return FNiagaraEditError::Make(TEXT("EVENT_HANDLER_NOT_FOUND"), FString::Printf(TEXT("Event handler with usageId '%s' was not found."), *TargetSpec.EntryId));
                }
            }
            else
            {
                if (TargetSpec.Index < 0 || TargetSpec.Index >= Handlers.Num())
                {
                    return FNiagaraEditError::Make(TEXT("EVENT_HANDLER_INVALID_INDEX"), FString::Printf(TEXT("Event handler index %d is out of range."), TargetSpec.Index));
                }
                ResolvedIndex = TargetSpec.Index;
            }
            OutTarget.EventHandlerIndex = ResolvedIndex;
            OutTarget.ReflectedStruct = FNiagaraEventScriptProperties::StaticStruct();
            OutTarget.ReflectedContainer = &Handlers[ResolvedIndex];
            return FNiagaraEditError();
        }

        case ENiagaraEditTargetKind::SimulationStage:
        {
            if (!OutTarget.EmitterData)
            {
                return FNiagaraEditError::Make(TEXT("EMITTER_DATA_MISSING"), TEXT("Simulation stage target requires versioned emitter data."));
            }
            const TArray<UNiagaraSimulationStageBase*>& Stages = OutTarget.EmitterData->GetSimulationStages();
            UNiagaraSimulationStageBase* ResolvedStage = nullptr;
            int32 ResolvedIndex = INDEX_NONE;
            if (!TargetSpec.EntryId.IsEmpty())
            {
                FGuid UsageId;
                if (!FGuid::Parse(TargetSpec.EntryId, UsageId))
                {
                    return FNiagaraEditError::Make(TEXT("SIMULATION_STAGE_INVALID_INDEX"), FString::Printf(TEXT("Simulation stage entryId '%s' is not a valid Guid."), *TargetSpec.EntryId));
                }
                ResolvedStage = OutTarget.EmitterData->GetSimulationStageById(UsageId);
                if (!ResolvedStage)
                {
                    return FNiagaraEditError::Make(TEXT("SIMULATION_STAGE_NOT_FOUND"), FString::Printf(TEXT("Simulation stage with usageId '%s' was not found."), *TargetSpec.EntryId));
                }
                ResolvedIndex = Stages.IndexOfByKey(ResolvedStage);
            }
            else
            {
                if (TargetSpec.Index < 0 || TargetSpec.Index >= Stages.Num())
                {
                    return FNiagaraEditError::Make(TEXT("SIMULATION_STAGE_INVALID_INDEX"), FString::Printf(TEXT("Simulation stage index %d is out of range."), TargetSpec.Index));
                }
                ResolvedStage = Stages[TargetSpec.Index];
                ResolvedIndex = TargetSpec.Index;
            }
            if (!ResolvedStage)
            {
                return FNiagaraEditError::Make(TEXT("SIMULATION_STAGE_NOT_FOUND"), TEXT("Resolved simulation stage is null."));
            }
            OutTarget.SimulationStageIndex = ResolvedIndex;
            OutTarget.ReflectedObject = ResolvedStage;
            return FNiagaraEditError();
        }

        default:
            return FNiagaraEditError::Make(TEXT("INVALID_TARGET_KIND"), FString::Printf(TEXT("Unknown Niagara edit target kind '%s'."), *TargetSpec.KindText));
        }
    }

    FNiagaraEditError ValidatePropertyPayload(const FNiagaraPropertyEditPayload& Payload, FNiagaraResolvedTarget& OutTarget, FProperty*& OutProperty, void*& OutContainer)
    {
        if (FNiagaraEditError Error = ResolveTarget(Payload.AssetPath, Payload.Target, OutTarget); Error.HasError())
        {
            return Error;
        }

        FString PropertyError;
        if (OutTarget.ReflectedObject)
        {
            OutProperty = ResolveNestedPropertyPath(OutTarget.ReflectedObject, Payload.PropertyPath, OutContainer, PropertyError);
        }
        else if (OutTarget.ReflectedStruct && OutTarget.ReflectedContainer)
        {
            TArray<FString> Segments;
            Payload.PropertyPath.ParseIntoArray(Segments, TEXT("."), true);
            UStruct* CurrentStruct = OutTarget.ReflectedStruct;
            void* CurrentContainer = OutTarget.ReflectedContainer;
            for (int32 Index = 0; Index < Segments.Num(); ++Index)
            {
                const bool bIsLeaf = Index == Segments.Num() - 1;
                OutProperty = FindPropertyCI(CurrentStruct, Segments[Index]);
                if (!OutProperty)
                {
                    PropertyError = FString::Printf(TEXT("Property '%s' not found in scope '%s'."), *Segments[Index], *CurrentStruct->GetName());
                    break;
                }
                if (bIsLeaf)
                {
                    OutContainer = CurrentContainer;
                    break;
                }
                FStructProperty* StructProperty = CastField<FStructProperty>(OutProperty);
                if (!StructProperty)
                {
                    PropertyError = FString::Printf(TEXT("Cannot traverse into property '%s'."), *Segments[Index]);
                    OutProperty = nullptr;
                    break;
                }
                CurrentContainer = StructProperty->ContainerPtrToValuePtr<void>(CurrentContainer);
                CurrentStruct = StructProperty->Struct;
            }
        }
        else
        {
            // Echo the kind the caller actually typed (KindText, always set by ParseTargetSpec),
            // so the message never names a kind the caller didn't pass. Only container-less
            // successful resolves reach here — parameterStore (ResolveParameterStore sets only
            // ParameterStore) and graph (ResolveGraphTarget sets only Graph); every other kind
            // either populates ReflectedObject/ReflectedStruct above or returns early. Steer the
            // parameterStore caller toward niagara.set_parameter, which is what actually writes
            // parameter-store values.
            FString Message = FString::Printf(TEXT("Target kind '%s' does not expose reflected properties."), *Payload.Target.KindText);
            if (Payload.Target.Kind == ENiagaraEditTargetKind::ParameterStore)
            {
                Message += TEXT(" Use niagara.set_parameter to set parameter-store values.");
            }
            return FNiagaraEditError::Make(TEXT("UNSUPPORTED_TARGET"), Message);
        }

        if (!OutProperty || !OutContainer)
        {
            return FNiagaraEditError::Make(TEXT("PROPERTY_NOT_FOUND"), PropertyError.IsEmpty() ? FString::Printf(TEXT("Property '%s' was not found."), *Payload.PropertyPath) : PropertyError);
        }
        return FNiagaraEditError();
    }

    FNiagaraEditError ValidateParameterPayload(const FNiagaraParameterEditPayload& Payload, ENiagaraEditOperation Operation, FNiagaraResolvedTarget& OutTarget)
    {
        FNiagaraEditTargetSpec Target;
        Target.Kind = ENiagaraEditTargetKind::ParameterStore;
        Target.KindText = TEXT("parameterStore");
        Target.Scope = Payload.Scope;
        Target.EmitterName = Payload.EmitterName;
        if (FNiagaraEditError Error = ResolveTarget(Payload.AssetPath, Target, OutTarget); Error.HasError())
        {
            return Error;
        }

        if (Operation == ENiagaraEditOperation::RemoveParameter)
        {
            return FNiagaraEditError();
        }
        return ValidateTypedValue(Payload.Type, Payload.Value, Operation == ENiagaraEditOperation::AddParameter ? TEXT("defaultValue") : TEXT("value"));
    }

    FNiagaraEditError ValidateRendererPayload(const FNiagaraRendererEditPayload& Payload, ENiagaraEditOperation Operation, FNiagaraResolvedTarget& OutTarget)
    {
        FNiagaraEditTargetSpec Target = Payload.Target;
        if (Operation == ENiagaraEditOperation::AddRenderer)
        {
            Target.Kind = ENiagaraEditTargetKind::EmitterData;
        }
        if (FNiagaraEditError Error = ResolveTarget(Payload.AssetPath, Target, OutTarget); Error.HasError())
        {
            return Error;
        }
        if (Operation == ENiagaraEditOperation::MoveRenderer && OutTarget.EmitterData)
        {
            const int32 RendererCount = OutTarget.EmitterData->GetRenderers().Num();
            if (Payload.Target.ToIndex < 0 || Payload.Target.ToIndex >= RendererCount)
            {
                return FNiagaraEditError::Make(TEXT("RENDERER_INDEX_INVALID"), FString::Printf(TEXT("Renderer toIndex %d is out of range."), Payload.Target.ToIndex));
            }
        }
        return FNiagaraEditError();
    }

    FNiagaraEditError ValidateModulePayload(const FNiagaraModuleEditPayload& Payload, ENiagaraEditOperation Operation, FNiagaraResolvedTarget& OutTarget)
    {
        FNiagaraEditTargetSpec Target = Payload.Target;
        Target.Kind = Operation == ENiagaraEditOperation::AddModule ? ENiagaraEditTargetKind::Graph : ENiagaraEditTargetKind::Module;
        return ResolveTarget(Payload.AssetPath, Target, OutTarget);
    }

    FNiagaraEditError ParseEventHandlerPayload(const TSharedPtr<FJsonObject>& Payload, bool bRequireIdentity, FNiagaraEventHandlerEditPayload& OutPayload)
    {
        if (FNiagaraEditError Error = RejectBatchPayload(Payload); Error.HasError())
        {
            return Error;
        }
        OutPayload.AssetPath = GetStringField(Payload, TEXT("assetPath"));
        if (OutPayload.AssetPath.IsEmpty())
        {
            return FNiagaraEditError::Make(TEXT("INVALID_ARGUMENT"), TEXT("Missing 'assetPath'."));
        }
        OutPayload.EmitterName = GetStringField(Payload, TEXT("emitter"));
        if (OutPayload.EmitterName.IsEmpty())
        {
            OutPayload.EmitterName = GetStringField(Payload, TEXT("emitterName"));
        }
        OutPayload.EntryId = GetStringField(Payload, TEXT("eventHandlerId"));
        if (OutPayload.EntryId.IsEmpty())
        {
            OutPayload.EntryId = GetStringField(Payload, TEXT("entryId"));
        }
        TryGetIntField(Payload, TEXT("eventHandlerIndex"), OutPayload.Index);
        if (OutPayload.Index == INDEX_NONE)
        {
            TryGetIntField(Payload, TEXT("index"), OutPayload.Index);
        }
        OutPayload.Source = GetStringField(Payload, TEXT("source"));
        OutPayload.ExecutionMode = GetStringField(Payload, TEXT("executionMode"));
        OutPayload.SourceEventName = GetStringField(Payload, TEXT("sourceEventName"));
        TryGetIntField(Payload, TEXT("spawnNumber"), OutPayload.SpawnNumber);
        TryGetIntField(Payload, TEXT("maxEventsPerFrame"), OutPayload.MaxEventsPerFrame);
        TryGetIntField(Payload, TEXT("minSpawnNumber"), OutPayload.MinSpawnNumber);
        if (Payload.IsValid() && Payload->HasField(TEXT("bRandomSpawnNumber")))
        {
            OutPayload.bHasRandomSpawnNumber = Payload->TryGetBoolField(TEXT("bRandomSpawnNumber"), OutPayload.bRandomSpawnNumber);
        }
        else if (Payload.IsValid() && Payload->HasField(TEXT("randomSpawnNumber")))
        {
            OutPayload.bHasRandomSpawnNumber = Payload->TryGetBoolField(TEXT("randomSpawnNumber"), OutPayload.bRandomSpawnNumber);
        }
        OutPayload.Options = ParseOptions(Payload);

        if (bRequireIdentity && OutPayload.EntryId.IsEmpty() && OutPayload.Index == INDEX_NONE)
        {
            return FNiagaraEditError::Make(TEXT("INVALID_ARGUMENT"), TEXT("Missing 'eventHandlerId' or 'eventHandlerIndex'."));
        }
        return FNiagaraEditError();
    }

    FNiagaraEditError ParseSimulationStagePayload(const TSharedPtr<FJsonObject>& Payload, bool bRequireIdentity, FNiagaraSimulationStageEditPayload& OutPayload)
    {
        if (FNiagaraEditError Error = RejectBatchPayload(Payload); Error.HasError())
        {
            return Error;
        }
        OutPayload.AssetPath = GetStringField(Payload, TEXT("assetPath"));
        if (OutPayload.AssetPath.IsEmpty())
        {
            return FNiagaraEditError::Make(TEXT("INVALID_ARGUMENT"), TEXT("Missing 'assetPath'."));
        }
        OutPayload.EmitterName = GetStringField(Payload, TEXT("emitter"));
        if (OutPayload.EmitterName.IsEmpty())
        {
            OutPayload.EmitterName = GetStringField(Payload, TEXT("emitterName"));
        }
        OutPayload.EntryId = GetStringField(Payload, TEXT("stageId"));
        if (OutPayload.EntryId.IsEmpty())
        {
            OutPayload.EntryId = GetStringField(Payload, TEXT("entryId"));
        }
        TryGetIntField(Payload, TEXT("stageIndex"), OutPayload.Index);
        if (OutPayload.Index == INDEX_NONE)
        {
            TryGetIntField(Payload, TEXT("index"), OutPayload.Index);
        }
        OutPayload.StageClassPath = GetStringField(Payload, TEXT("stageClass"));
        if (OutPayload.StageClassPath.IsEmpty())
        {
            OutPayload.StageClassPath = GetStringField(Payload, TEXT("stageClassPath"));
        }
        if (OutPayload.StageClassPath.IsEmpty())
        {
            OutPayload.StageClassPath = GetStringField(Payload, TEXT("classPath"));
        }
        TryGetIntField(Payload, TEXT("atIndex"), OutPayload.AtIndex);
        OutPayload.Options = ParseOptions(Payload);

        if (bRequireIdentity && OutPayload.EntryId.IsEmpty() && OutPayload.Index == INDEX_NONE)
        {
            return FNiagaraEditError::Make(TEXT("INVALID_ARGUMENT"), TEXT("Missing 'stageId' or 'stageIndex'."));
        }
        return FNiagaraEditError();
    }

    FNiagaraEditError ParseDataInterfacePayload(const TSharedPtr<FJsonObject>& Payload, bool bRequireClass, FNiagaraDataInterfaceEditPayload& OutPayload)
    {
        if (FNiagaraEditError Error = RejectBatchPayload(Payload); Error.HasError())
        {
            return Error;
        }
        OutPayload.AssetPath = GetStringField(Payload, TEXT("assetPath"));
        if (OutPayload.AssetPath.IsEmpty())
        {
            return FNiagaraEditError::Make(TEXT("INVALID_ARGUMENT"), TEXT("Missing 'assetPath'."));
        }
        OutPayload.Scope = GetStringField(Payload, TEXT("scope"));
        if (OutPayload.Scope.IsEmpty())
        {
            return FNiagaraEditError::Make(TEXT("INVALID_ARGUMENT"), TEXT("Missing 'scope'."));
        }
        OutPayload.EmitterName = GetStringField(Payload, TEXT("emitter"));
        if (OutPayload.EmitterName.IsEmpty())
        {
            OutPayload.EmitterName = GetStringField(Payload, TEXT("emitterName"));
        }
        OutPayload.ParameterName = GetStringField(Payload, TEXT("parameterName"));
        if (OutPayload.ParameterName.IsEmpty())
        {
            OutPayload.ParameterName = GetStringField(Payload, TEXT("name"));
        }
        if (OutPayload.ParameterName.IsEmpty())
        {
            return FNiagaraEditError::Make(TEXT("INVALID_ARGUMENT"), TEXT("Missing 'parameterName'."));
        }
        OutPayload.DataInterfaceClassPath = GetStringField(Payload, TEXT("dataInterfaceClass"));
        if (OutPayload.DataInterfaceClassPath.IsEmpty())
        {
            OutPayload.DataInterfaceClassPath = GetStringField(Payload, TEXT("dataInterfaceClassPath"));
        }
        if (OutPayload.DataInterfaceClassPath.IsEmpty())
        {
            OutPayload.DataInterfaceClassPath = GetStringField(Payload, TEXT("classPath"));
        }
        OutPayload.Options = ParseOptions(Payload);

        if (bRequireClass && OutPayload.DataInterfaceClassPath.IsEmpty())
        {
            return FNiagaraEditError::Make(TEXT("INVALID_ARGUMENT"), TEXT("Missing 'dataInterfaceClass'."));
        }
        return FNiagaraEditError();
    }

    UClass* ResolveNiagaraSubclassByPath(UClass* BaseClass, const FString& ClassPath, const TCHAR* DefaultModule)
    {
        if (!BaseClass || ClassPath.IsEmpty())
        {
            return nullptr;
        }

        if (UClass* Class = FSoftClassPath(ClassPath).TryLoadClass<UObject>())
        {
            if (Class->IsChildOf(BaseClass))
            {
                return Class;
            }
        }
        if (UClass* Class = FindObject<UClass>(nullptr, *ClassPath))
        {
            if (Class->IsChildOf(BaseClass))
            {
                return Class;
            }
        }
        if (UClass* Class = StaticLoadClass(BaseClass, nullptr, *ClassPath))
        {
            return Class;
        }
        if (DefaultModule && *DefaultModule && !ClassPath.Contains(TEXT("/")) && !ClassPath.Contains(TEXT(".")))
        {
            const FString QualifiedPath = FString::Printf(TEXT("/Script/%s.%s"), DefaultModule, *ClassPath);
            if (UClass* Class = StaticLoadClass(BaseClass, nullptr, *QualifiedPath))
            {
                return Class;
            }
        }
        return nullptr;
    }

    FNiagaraEditError ValidatePinPayload(const FNiagaraPinEditPayload& Payload, ENiagaraEditOperation Operation, FNiagaraResolvedTarget& OutTarget)
    {
        FNiagaraEditTargetSpec Target = Payload.Target;
        Target.Kind = ENiagaraEditTargetKind::Graph;
        if (FNiagaraEditError Error = ResolveTarget(Payload.AssetPath, Target, OutTarget); Error.HasError())
        {
            return Error;
        }

        if (Operation == ENiagaraEditOperation::SetPinDefault)
        {
            UEdGraphNode* Node = FindNode(OutTarget.Graph, Payload.NodeId);
            if (!Node)
            {
                return FNiagaraEditError::Make(TEXT("NODE_NOT_FOUND"), FString::Printf(TEXT("Node '%s' was not found."), *Payload.NodeId));
            }
            UEdGraphPin* Pin = FindPin(Node, Payload.PinName);
            if (!Pin)
            {
                return FNiagaraEditError::Make(TEXT("PIN_NOT_FOUND"), FString::Printf(TEXT("Pin '%s' was not found."), *Payload.PinName));
            }
            OutTarget.Node = Node;
            OutTarget.Pin = Pin;
            return FNiagaraEditError();
        }

        UEdGraphNode* FromNode = FindNode(OutTarget.Graph, Payload.FromNode);
        UEdGraphNode* ToNode = FindNode(OutTarget.Graph, Payload.ToNode);
        if (!FromNode || !ToNode)
        {
            return FNiagaraEditError::Make(TEXT("NODE_NOT_FOUND"), TEXT("Source or destination node was not found."));
        }
        if (!FindPin(FromNode, Payload.FromPin) || !FindPin(ToNode, Payload.ToPin))
        {
            return FNiagaraEditError::Make(TEXT("PIN_NOT_FOUND"), TEXT("Source or destination pin was not found."));
        }
        return FNiagaraEditError();
    }

    FGuid ResolveEmitterVersionGuid(const FNiagaraResolvedTarget& Target)
    {
        if (Target.EmitterData)
        {
            return Target.EmitterData->Version.VersionGuid;
        }
        if (Target.Emitter)
        {
            return Target.Emitter->GetExposedVersion().VersionGuid;
        }
        return FGuid();
    }

    void RecordDataInterfaceVerdictBefore(FNiagaraResolvedTarget& Target)
    {
        if (Target.bDataInterfaceVerdictBeforeMeasured || !Target.System)
        {
            return;
        }
        TArray<PinWrightNiagara::FDataInterfaceCountMismatch> Scratch;
        Target.DataInterfaceVerdictBefore =
            PinWrightNiagara::CheckDataInterfaceCounts(*Target.System, Scratch);
        Target.bDataInterfaceVerdictBeforeMeasured = true;
    }

    void ModifyResolvedTarget(FNiagaraResolvedTarget& Target)
    {
        RecordDataInterfaceVerdictBefore(Target);
        if (Target.Asset)
        {
            Target.Asset->Modify();
        }
        if (Target.System && Target.System != Target.Asset)
        {
            Target.System->Modify();
        }
        if (Target.Emitter && Target.Emitter != Target.Asset)
        {
            Target.Emitter->Modify();
        }
        if (Target.ReflectedObject
            && Target.ReflectedObject != Target.Asset
            && Target.ReflectedObject != Target.System
            && Target.ReflectedObject != Target.Emitter)
        {
            Target.ReflectedObject->Modify();
        }
        UObject* RendererObject = Target.Renderer;
        if (Target.Renderer
            && RendererObject != Target.ReflectedObject
            && RendererObject != Target.Asset
            && RendererObject != Target.System
            && RendererObject != Target.Emitter)
        {
            Target.Renderer->Modify();
        }
    }

    void NotifyNiagaraObjectChanged(const FNiagaraResolvedTarget& Target, FProperty* ChangedProperty)
    {
        if (Target.ReflectedObject)
        {
            if (ChangedProperty)
            {
                FPropertyChangedEvent ChangedEvent(ChangedProperty, EPropertyChangeType::ValueSet);
                Target.ReflectedObject->PostEditChangeProperty(ChangedEvent);
            }
            else
            {
                Target.ReflectedObject->PostEditChange();
            }
        }

        if (Target.EmitterData)
        {
            Target.EmitterData->InvalidateCompileResults();
        }
        if (Target.Emitter)
        {
            Target.Emitter->MarkPackageDirty();
        }
        if (Target.System)
        {
            Target.System->MarkPackageDirty();
        }
        if (Target.Asset)
        {
            Target.Asset->MarkPackageDirty();
        }
    }

    bool RequestNiagaraCompile(FNiagaraResolvedTarget& Target, bool bForce)
    {
        Target.CompileRequestSystems.Reset();
        if (Target.System)
        {
            // Quiesce first: a recompile swaps the compiled scripts out from under any
            // FNiagaraSystemInstance still ticking this system on a task-graph worker, and the
            // new bytecode indexing the old exec context's data sets asserts inside the
            // VectorVM — an appError, so the editor process dies. Six sibling call sites
            // already honour this invariant before mutating; the shared compile path is the
            // one that did not. See PinWrightNiagara::KillSystemInstances.
            //
            // Accumulated, not assigned: BeginEmitterMutationScope may already have stopped
            // instances for this same call, and the response reports the call's total.
            Target.QuiescedInstances += PinWrightNiagara::KillSystemInstances(*Target.System);

            // UNiagaraSystem::RequestCompile(true) routes through
            // ForceGraphToRecompileOnNextCheck() at NiagaraSystem.cpp:2968,
            // which performs `SystemSpawnScript->GetLatestSource()->ForceGraphToRecompileOnNextCheck()`.
            // On a bare NewObject<UNiagaraSystem>(GetTransientPackage()),
            // PostInitProperties (line 553) creates SystemSpawnScript but
            // never calls SetLatestSource on it — that wiring happens in
            // PostLoad (line 706). So the inner GetLatestSource() returns
            // null and the chained call crashes. Guard on the *source* of
            // both system scripts (and the check() on line 2967 compares
            // SystemSpawnScript and SystemUpdateScript latest sources).
            if (HasSystemCompileSources(*Target.System))
            {
                // RequestCompile returns bLaunchedCompilations, so a system the engine
                // considers current (the `force:false` case) reports false here rather than
                // claiming a compile that never ran.
                const bool bRequested = Target.System->RequestCompile(bForce);
                if (bRequested)
                {
                    Target.CompileRequestSystems.Add(Target.System);
                }
                return bRequested;
            }

            // Bare/transient system: no engine request was safe or possible, so it is not a
            // completed compile. The old true return made the synthetic no-op indistinguishable
            // from a compile that actually landed.
            return false;
        }

        if (Target.Emitter)
        {
            const FGuid VersionGuid = ResolveEmitterVersionGuid(Target);
            // Same hazard, wider blast radius: RequestCompileForEmitter recompiles every
            // loaded system that uses this emitter, so every live instance of those systems
            // has to be quiesced, not just the ones playing the edited asset.
            Target.QuiescedInstances += PinWrightNiagara::KillSystemInstancesUsingEmitter(*Target.Emitter, VersionGuid);
            const FVersionedNiagaraEmitter VersionedEmitter(Target.Emitter, VersionGuid);
            for (TObjectIterator<UNiagaraSystem> It; It; ++It)
            {
                UNiagaraSystem* System = *It;
                if (System
                    && System->UsesEmitter(VersionedEmitter)
                    && HasSystemCompileSources(*System)
                    && System->RequestCompile(bForce))
                {
                    Target.CompileRequestSystems.Add(System);
                }
            }
            return Target.CompileRequestSystems.Num() > 0;
        }

        return false;
    }

    bool MayPersistAfterDataInterfaceCheck(PinWrightNiagara::EDataInterfaceConsistency Verdict)
    {
        return Verdict != PinWrightNiagara::EDataInterfaceConsistency::Mismatched;
    }

    const TCHAR* DataInterfaceRepairToString(FNiagaraResolvedTarget::EDataInterfaceRepair Repair)
    {
        switch (Repair)
        {
        case FNiagaraResolvedTarget::EDataInterfaceRepair::Recompiled:
            return TEXT("recompiled");
        case FNiagaraResolvedTarget::EDataInterfaceRepair::Failed:
            return TEXT("failed");
        default:
            return TEXT("not_needed");
        }
    }

    void AddDataInterfaceDelta(
        const TSharedPtr<FJsonObject>& Result,
        bool bBeforeMeasured,
        PinWrightNiagara::EDataInterfaceConsistency Before,
        PinWrightNiagara::EDataInterfaceConsistency After,
        FNiagaraResolvedTarget::EDataInterfaceRepair Repair,
        int32 LiveInstancesAtRepair)
    {
        if (!Result.IsValid())
        {
            return;
        }
        TSharedPtr<FJsonObject> Delta = MakeShared<FJsonObject>();
        Delta->SetStringField(TEXT("before"), bBeforeMeasured
            ? PinWrightNiagara::DataInterfaceConsistencyToString(Before)
            : TEXT("unmeasured"));
        Delta->SetStringField(TEXT("after"),
            PinWrightNiagara::DataInterfaceConsistencyToString(After));
        Delta->SetBoolField(TEXT("changed"), bBeforeMeasured && Before != After);
        // The attribution the whole field exists for. False on an inherited mismatch and false on
        // an unmeasured before, because neither is evidence this call caused anything.
        Delta->SetBoolField(TEXT("armedByThisWrite"),
            bBeforeMeasured && PinWrightNiagara::DidWriteArmDataInterfaceMismatch(Before, After));
        Delta->SetStringField(TEXT("repair"), DataInterfaceRepairToString(Repair));
        if (Repair != FNiagaraResolvedTarget::EDataInterfaceRepair::NotNeeded)
        {
            Delta->SetNumberField(TEXT("liveInstancesAtRepair"), LiveInstancesAtRepair);
        }
        Result->SetObjectField(TEXT("dataInterfaceDelta"), Delta);
    }

    void AddNiagaraAssetSaveReport(
        const TSharedPtr<FJsonObject>& Result,
        bool bSaveRequested,
        bool bSavedToDisk,
        EAssetSaveState SaveState)
    {
        AddAssetSaveReport(Result, bSaveRequested, bSavedToDisk, TOptional<EAssetSaveState>(SaveState));
        if (Result && bSaveRequested && !bSavedToDisk)
        {
            // The shared helper keeps pendingFlush:true for every non-durable result for legacy
            // callers. Niagara has a typed state on every path, so it can be precise: only a
            // Deferred write is flushable; Failed and the other terminal/refused states require
            // their saveDetail remedy instead.
            Result->SetBoolField(TEXT("pendingFlush"), SaveState == EAssetSaveState::Deferred);
        }
    }

    bool FinalizeNiagaraEdit(FNiagaraResolvedTarget& Target, const FNiagaraEditOptions& Options, bool& bOutCompiled, bool& bOutSaved)
    {
        bOutCompiled = false;
        bOutSaved = false;
        Target.SaveState = EAssetSaveState::NotRequested;
        bool bCompileIssued = false;
        if (Options.bCompile)
        {
            // Always forced here: this runs immediately after a mutation, so the graph the
            // engine last compiled is known stale regardless of what its change ids say.
            bCompileIssued = RequestNiagaraCompile(Target, /*bForce=*/true);
        }

        // The `{compile: true, save: true}` pair on one call is a data-corrupting race, and it
        // is the most common call shape in this namespace. RequestCompile is asynchronous, so
        // the save below used to run while the compile was still in flight; UNiagaraScript's
        // presave then found the script's compiled DataInterfaceInfo list empty while the
        // emitter resolved N of them, logged "Data interface count mismatch during script
        // presave. Invaliding compile results" and wrote the invalidated result into the
        // .uasset. Anything that re-ticked the system afterwards asserted in the VectorVM on a
        // worker thread and killed the editor. Measured on one system: 44 mismatch warnings
        // across ~25 paired edits, zero once the compile was allowed to land before the save.
        //
        // Scoped to exactly that pair: a compile-only edit stays fire-and-forget and therefore
        // reports compiled:false (niagara.compile / compile_status are the completion surface),
        // while a save-only edit has nothing in flight to wait for.
        bool bCompileIsPersistable = true;
        if (Options.bCompile && Options.bSave)
        {
            const PinWrightNiagara::FCompileWaitOutcome Wait = Target.Emitter
                ? PinWrightNiagara::WaitForEmitterCompile(
                    *Target.Emitter,
                    ResolveEmitterVersionGuid(Target),
                    Target.CompileRequestSystems)
                : PinWrightNiagara::WaitForRequestedSystemCompiles(Target.CompileRequestSystems);

            // Where the wait ran, `compiled` reports the observation rather than the request.
            bOutCompiled = PinWrightNiagara::DidCompileLand(bCompileIssued, Wait);
            bCompileIsPersistable = bOutCompiled && PinWrightNiagara::MayPersistAfterCompileWait(Wait);
            if (!bCompileIsPersistable)
            {
                // Refusing the save is the point: persisting here is what corrupts the asset,
                // and `saved:false` beside `saveRequested:true` is the honest report. The
                // response's saveState/saveDetail explains that retrying is required.
                Target.SaveState = EAssetSaveState::Failed;
                UE_LOG(LogPinWrightSubsystem, Warning,
                    TEXT("niagara edit on '%s': no requested compile was observed complete (waited %.1f s); ")
                    TEXT("refusing to save an invalidated compile. Re-run niagara.compile then asset.save."),
                    *Target.AssetPath,
                    Wait.WaitedSeconds);
            }
        }

        // The data-interface gate, for the thirty mutation verbs that save through here.
        //
        // A UNiagaraSystem whose compiled data-interface count disagrees with its resolved one is
        // fatal on its NEXT tick: the bytecode indexes a data set the execution context never
        // allocated and the VectorVM asserts (`DataSetIdx < ExecCtx->DataSets.Num()`) on a
        // concurrent worker, which is an appError and takes the editor process down — minutes
        // later, from an unrelated caller that merely forced a re-tick. niagara.add_emitter and
        // niagara.remove_emitter already refuse to persist that state; every other mutating verb
        // in the namespace reaches disk through this function and did not, so `saved: true` came
        // back on assets the plugin's own checker classifies as fatal-on-tick
        // (B-niagara-finalize-edit-no-di-gate).
        //
        // After the wait, not before it: a landed compile is what clears a mismatch, so checking
        // ahead of the wait would refuse writes the compile has just made safe.
        //
        // Recorded on the target rather than returned, so MakeMutationResult can state the cause
        // in the same envelope — `saved: false` with nothing said is the defect
        // `quiescedInstances` was added to end, not a fix for it.
        if (Target.System)
        {
            Target.DataInterfaceVerdict =
                PinWrightNiagara::CheckDataInterfaceCounts(*Target.System, Target.DataInterfaceMismatches);
        }

        // Refusing the SAVE is not enough when something is already ticking the system.
        //
        // The arming step is in-memory and unconditional: an emitter-scoped edit reaches
        // NotifyNiagaraObjectChanged, which calls FVersionedNiagaraEmitterData::InvalidateCompileResults,
        // which resets each of that emitter's scripts' FNiagaraVMExecutableData. The system's
        // serialized ScriptRuntimeCompiledDataForEditor still holds N resolved entries, so the
        // counts part company the instant the write lands - value-identically, on first touch,
        // one emitter per call. Nothing has to be saved for that to kill the editor; the next
        // tick of a live instance is enough.
        //
        // WHY THIS RECOMPILES RATHER THAN REFUSING. The resolved set is rebuilt in exactly one
        // place, UNiagaraSystem::InitScriptCompiledData, and that has exactly one caller:
        // ProcessCompilationResult, after a compile completes (NiagaraSystem.cpp). There is no
        // engine entry point that re-resolves without compiling, so "re-resolve it" and "compile
        // it" are the same operation - and refusing instead would mean refusing an ordinary
        // parameter write on any system an actor in the level happens to be playing, which is the
        // common case rather than the exceptional one. The engine's own editor takes the same
        // route: it recompiles and reinitializes rather than leaving a live system holding
        // invalidated scripts.
        //
        // NARROW BY CONSTRUCTION, because a compile is not free and resets system-scope
        // rapid-iteration values (B-niagara-force-compile-resets-rapid-iteration-values). It runs
        // only when THIS write moved a system that really was compared from a pass to a mismatch,
        // AND something holds a live FNiagaraSystemInstance for it. With nothing live, the
        // measured batch workflow - many edits, then one compile - stays correct and is not
        // charged for a compile per edit.
        if (Target.System
            && Target.bDataInterfaceVerdictBeforeMeasured
            && PinWrightNiagara::DidWriteArmDataInterfaceMismatch(
                Target.DataInterfaceVerdictBefore, Target.DataInterfaceVerdict))
        {
            Target.DataInterfaceLiveInstances = PinWrightNiagara::CountLiveSystemInstances(*Target.System);
            if (Target.DataInterfaceLiveInstances > 0)
            {
                // RequestNiagaraCompile quiesces before it asks, so the detonator is gone the
                // moment this returns even if the compile itself never lands. That is the half of
                // the repair that cannot fail, and it is the half the editor's survival depends on.
                const bool bRepairIssued = RequestNiagaraCompile(Target, /*bForce=*/true);
                const PinWrightNiagara::FCompileWaitOutcome RepairWait =
                    PinWrightNiagara::WaitForRequestedSystemCompiles(Target.CompileRequestSystems);
                if (PinWrightNiagara::DidCompileLand(bRepairIssued, RepairWait))
                {
                    // A compile really did run and land, so `compiled` must say so even though the
                    // caller did not ask for one. `compileRequested: false` beside `compiled: true`
                    // is the honest pairing, and dataInterfaceDelta.repair names who asked; denying
                    // the compile here would leave the envelope contradicting itself.
                    bOutCompiled = true;
                }
                Target.DataInterfaceVerdict = PinWrightNiagara::CheckDataInterfaceCounts(
                    *Target.System, Target.DataInterfaceMismatches);
                Target.DataInterfaceRepair =
                    Target.DataInterfaceVerdict != PinWrightNiagara::EDataInterfaceConsistency::Mismatched
                        ? FNiagaraResolvedTarget::EDataInterfaceRepair::Recompiled
                        : FNiagaraResolvedTarget::EDataInterfaceRepair::Failed;
                if (Target.DataInterfaceRepair == FNiagaraResolvedTarget::EDataInterfaceRepair::Failed)
                {
                    UE_LOG(LogPinWrightSubsystem, Warning,
                        TEXT("niagara edit on '%s': this write moved the system to a data-interface mismatch ")
                        TEXT("while %d live instance(s) were running it. The instances were stopped and a ")
                        TEXT("compile was requested (issued=%s, waited %.1f s), and the system is STILL ")
                        TEXT("mismatched (%s). Nothing ticks it now, but it must not be re-activated or saved ")
                        TEXT("until niagara.compile succeeds."),
                        *Target.AssetPath,
                        Target.DataInterfaceLiveInstances,
                        bRepairIssued ? TEXT("true") : TEXT("false"),
                        RepairWait.WaitedSeconds,
                        *PinWrightNiagara::DescribeDataInterfaceMismatches(Target.DataInterfaceMismatches));
                }
            }
        }

        const bool bDataInterfacesArePersistable = MayPersistAfterDataInterfaceCheck(Target.DataInterfaceVerdict);
        if (Options.bSave && !bDataInterfacesArePersistable)
        {
            Target.SaveState = EAssetSaveState::Failed;
            UE_LOG(LogPinWrightSubsystem, Warning,
                TEXT("niagara edit on '%s': compiled and resolved data-interface counts disagree (%s). ")
                TEXT("Refusing to save a system that asserts inside the VectorVM on its next tick. ")
                TEXT("Run niagara.compile, or niagara.remove_orphan_data_interfaces, then asset.save."),
                *Target.AssetPath,
                *PinWrightNiagara::DescribeDataInterfaceMismatches(Target.DataInterfaceMismatches));
        }

        if (Options.bSave && Target.Asset && bCompileIsPersistable && bDataInterfacesArePersistable)
        {
            // Persist to disk for real (not the mark-dirty McpSafeAssetSave) and
            // gate bOutSaved on the .uasset actually landing on disk, so a
            // save:true edit cannot report saved:true for an asset never written
            // (B-niagara-save-no-disk-write).
            bOutSaved = SaveAssetToDiskReportingPresence(
                Target.Asset,
                /*bForce=*/true,
                /*OutPackageName=*/nullptr,
                /*OutSizeBytes=*/nullptr,
                &Target.SaveState);
        }
        else if (Options.bSave && Target.SaveState == EAssetSaveState::NotRequested)
        {
            Target.SaveState = EAssetSaveState::Failed;
        }
        return true;
    }

    TSharedPtr<FJsonObject> MakeMutationResult(
        const TCHAR* Operation,
        const FNiagaraResolvedTarget& Target,
        const FNiagaraEditOptions& Options,
        bool bCompiled,
        bool bSaved)
    {
        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetBoolField(TEXT("success"), true);
        Result->SetStringField(TEXT("operation"), Operation);
        Result->SetStringField(TEXT("assetPath"), Target.AssetPath);
        Result->SetStringField(TEXT("assetKind"), Target.AssetKind);
        Result->SetBoolField(TEXT("compileRequested"), Options.bCompile);
        Result->SetBoolField(TEXT("compiled"), bCompiled);
        AddNiagaraAssetSaveReport(Result, Options.bSave, bSaved, Target.SaveState);
        // Always present, including as 0. Mutating this namespace destroys the running system
        // instances of the edited asset — the preview viewport of anyone who has it open
        // included — and until this field existed the response said nothing about it, so from
        // the other side a viewport just went blank with no stated cause.
        Result->SetNumberField(TEXT("quiescedInstances"), Target.QuiescedInstances);
        // The owner the call actually resolved to, always present (empty for a system-scope or
        // standalone-emitter edit). `entryId` is a bare node guid that duplicated emitters share,
        // so a right id paired with a wrong `emitter` used to land on the wrong emitter with
        // nothing in the response to show it; `entryKey` repeats the id in the owner-qualified
        // form that cannot be replayed against the wrong emitter
        // (B-niagara-entry-id-not-unique-across-emitters).
        Result->SetStringField(TEXT("emitter"),
            Target.EmitterHandle ? Target.EmitterHandle->GetName().ToString() : FString());
        if (Target.ModuleNode)
        {
            Result->SetStringField(TEXT("entryKey"),
                MakeModuleEntryKey(ResolvedStackOwnerName(Target), Target.ModuleNode->NodeGuid));
        }
        // Always present, `"unverified"` included: a check that could not run must not be echoed
        // as one that passed, which is the whole reason the verdict is three-valued. An edit
        // addressed at a standalone Niagara Emitter asset always reports `"unverified"` —
        // ResolveTarget leaves Target.System null there and the check needs a system to compare.
        Result->SetStringField(TEXT("dataInterfaceCheck"),
            PinWrightNiagara::DataInterfaceConsistencyToString(Target.DataInterfaceVerdict));
        AddDataInterfaceDelta(
            Result,
            Target.bDataInterfaceVerdictBeforeMeasured,
            Target.DataInterfaceVerdictBefore,
            Target.DataInterfaceVerdict,
            Target.DataInterfaceRepair,
            Target.DataInterfaceLiveInstances);
        if (Target.DataInterfaceMismatches.Num() > 0)
        {
            // Same per-script shape niagara.add_emitter's NIAGARA_DATA_INTERFACE_MISMATCH payload
            // and niagara.inspect already publish, so the namespace has one vocabulary for one
            // fact and a caller can correlate a refusal here against those without translating.
            TArray<TSharedPtr<FJsonValue>> MismatchedScripts;
            MismatchedScripts.Reserve(Target.DataInterfaceMismatches.Num());
            for (const PinWrightNiagara::FDataInterfaceCountMismatch& Mismatch : Target.DataInterfaceMismatches)
            {
                TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
                Entry->SetStringField(TEXT("scriptPath"), Mismatch.ScriptPath);
                Entry->SetStringField(TEXT("emitter"), Mismatch.EmitterName);
                Entry->SetNumberField(TEXT("compiledDataInterfaces"), Mismatch.CompiledCount);
                Entry->SetNumberField(TEXT("resolvedDataInterfaces"), Mismatch.ResolvedCount);
                MismatchedScripts.Add(MakeShared<FJsonValueObject>(Entry));
            }
            Result->SetArrayField(TEXT("mismatchedScripts"), MismatchedScripts);
        }
        return Result;
    }

    void SnapshotInputOverrides(UNiagaraNodeFunctionCall& ModuleNode, TMap<FNiagaraVariable, FString>& OutSnapshot)
    {
        OutSnapshot.Empty();
        UEdGraphPin* ParameterMapInputPin = nullptr;
        {
            TArray<UEdGraphPin*> InputPins;
            ModuleNode.GetInputPins(InputPins);
            for (UEdGraphPin* Pin : InputPins)
            {
                if (!Pin)
                {
                    continue;
                }
                const UEdGraphSchema_Niagara* NiagaraSchema = Cast<UEdGraphSchema_Niagara>(Pin->GetSchema());
                if (NiagaraSchema && NiagaraSchema->PinToTypeDefinition(Pin) == FNiagaraTypeDefinition::GetParameterMapDef())
                {
                    ParameterMapInputPin = Pin;
                    break;
                }
            }
        }
        if (!ParameterMapInputPin || ParameterMapInputPin->LinkedTo.Num() == 0)
        {
            return;
        }

        UNiagaraNode* OverrideNode = Cast<UNiagaraNode>(ParameterMapInputPin->LinkedTo[0]->GetOwningNode());
        if (!OverrideNode)
        {
            return;
        }

        TArray<UEdGraphPin*> OverridePins;
        OverrideNode->GetInputPins(OverridePins);
        for (UEdGraphPin* Pin : OverridePins)
        {
            if (!Pin)
            {
                continue;
            }
            const FNiagaraParameterHandle Handle(Pin->PinName);
            if (Handle.GetNamespace() != FName(*ModuleNode.GetFunctionName()))
            {
                continue;
            }
            const FNiagaraTypeDefinition TypeDef = UEdGraphSchema_Niagara::PinToTypeDefinition(Pin);
            if (!TypeDef.IsValid())
            {
                continue;
            }
            FNiagaraVariable Var(TypeDef, Handle.GetName());
            OutSnapshot.Add(Var, Pin->DefaultValue);
        }
    }

    void EnumerateScriptInputs(UNiagaraScript& Script, TArray<FNiagaraVariable>& OutInputs)
    {
        OutInputs.Empty();
        UNiagaraScriptSource* Source = Cast<UNiagaraScriptSource>(Script.GetLatestSource());
        if (!Source)
        {
            return;
        }
        UNiagaraGraph* Graph = Source->NodeGraph;
        if (!Graph)
        {
            return;
        }

        // Collect exposed Parameter inputs only.
        // UNiagaraGraph::FindInputNodes is declared in the public header but not
        // NIAGARAEDITOR_API in UE 5.6, so it can't be linked from this DLL.
        // We replicate the FindOptions={bIncludeParameters=true, bIncludeAttributes=false,
        // bIncludeSystemConstants=false, bFilterDuplicates=true} subset by iterating the
        // graph nodes via UEdGraph::GetNodesOfClass (inline template) and filtering by
        // Usage==Parameter + IsExposed(). AddUnique gives us the bFilterDuplicates behaviour.
        TArray<UNiagaraNodeInput*> InputNodes;
        Graph->GetNodesOfClass<UNiagaraNodeInput>(InputNodes);
        for (const UNiagaraNodeInput* InputNode : InputNodes)
        {
            if (InputNode
                && InputNode->Usage == ENiagaraInputNodeUsage::Parameter
                && InputNode->IsExposed()
                && InputNode->Input.IsValid())
            {
                OutInputs.AddUnique(InputNode->Input);
            }
        }
    }

    void EnumerateModuleStackInputs(const UNiagaraNodeFunctionCall& ModuleNode, TArray<FNiagaraVariable>& OutInputs)
    {
        OutInputs.Empty();
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 6, 0)
        // ModuleInputsOnly filters to the module-namespace inputs (SpawnRate, Color, ...),
        // excluding engine/system constant reads. A default constant resolver is the
        // outside-a-live-system form GetStackFunctionInputs documents for graph-level reads;
        // it is sufficient here because we only need the declared input list, not
        // static-switch-resolved visibility.
        FNiagaraStackGraphUtilities::GetStackFunctionInputs(
            ModuleNode,
            OutInputs,
            FCompileConstantResolver(),
            FNiagaraStackGraphUtilities::ENiagaraGetStackFunctionInputPinsOptions::ModuleInputsOnly,
            /*bIgnoreDisabled=*/false);
#else
        // GetStackFunctionInputs is declared in the public header but is NOT NIAGARAEDITOR_API
        // before UE 5.6, so it cannot be linked from this DLL (LNK2019). Read the same set
        // directly off the called module graph instead: ModuleInputsOnly is exactly the
        // graph's "Module." namespace parameters, and UNiagaraGraph::GetAllMetaData() /
        // FindStaticSwitchInputs() ARE exported. Static-switch inputs are excluded because the
        // dump reports them separately (BuildStaticSwitchInputs).
        UNiagaraGraph* CalledGraph = const_cast<UNiagaraNodeFunctionCall&>(ModuleNode).GetCalledGraph();
        if (!CalledGraph)
        {
            return;
        }
        const TSet<FNiagaraVariable> StaticSwitchInputs(CalledGraph->FindStaticSwitchInputs());
        for (const TPair<FNiagaraVariable, TObjectPtr<UNiagaraScriptVariable>>& Pair : CalledGraph->GetAllMetaData())
        {
            const FNiagaraVariable& Variable = Pair.Key;
            if (Variable.IsInNameSpace(FNiagaraConstants::ModuleNamespaceString)
                && !StaticSwitchInputs.Contains(Variable))
            {
                OutInputs.AddUnique(Variable);
            }
        }
#endif
    }

    void ClassifyModuleInputBindings(UNiagaraNodeFunctionCall& ModuleNode, TMap<FName, FModuleInputBindingInfo>& OutByName)
    {
        OutByName.Empty();

        // Use the canonical override-node finder — the same one niagara.decompile_nir and
        // niagara.reset_module_input use — so this readback classifies each override pin
        // exactly as those paths do. Returns null when the module has no override node, i.e.
        // every input is at its script default.
        UNiagaraNodeParameterMapSet* OverrideNode =
            NiagaraResetModuleInput::FindStackFunctionOverrideNode(ModuleNode);
        if (!OverrideNode)
        {
            return;
        }

        const FName ModuleFunctionName(*ModuleNode.GetFunctionName());

        // UNiagaraNodeParameterMapGet is a private engine header (like the MapSet override
        // node); resolve its class by reflection to detect the modern linked-parameter wiring
        // where the override pin is fed by a Map Get output pin named for the bound parameter.
        static const UClass* MapGetClass =
            FindObject<UClass>(nullptr, TEXT("/Script/NiagaraEditor.NiagaraNodeParameterMapGet"));

        TArray<UEdGraphPin*> OverridePins;
        OverrideNode->GetInputPins(OverridePins);
        for (UEdGraphPin* Pin : OverridePins)
        {
            if (!Pin)
            {
                continue;
            }
            const FNiagaraParameterHandle Handle(Pin->PinName);
            if (Handle.GetNamespace() != ModuleFunctionName)
            {
                continue;
            }

            FModuleInputBindingInfo Info;
            if (Pin->LinkedTo.Num() > 0 && Pin->LinkedTo[0])
            {
                UEdGraphPin* UpstreamPin = Pin->LinkedTo[0];
                const UEdGraphNode* Upstream = UpstreamPin->GetOwningNode();
                // Classify exactly as the engine's authoritative override-pin classifier
                // UNiagaraStackFunctionInput::UpdateValuesFromOverridePin does, in the same
                // order (the ordering is load-bearing: CustomHlsl derives from FunctionCall).
                if (const UNiagaraNodeInput* LinkedInput = Cast<UNiagaraNodeInput>(Upstream))
                {
                    // A UNiagaraNodeInput override carries a data-interface value or an
                    // object-asset value — NEVER a linked parameter. (Linked parameters are
                    // always a UNiagaraNodeParameterMapGet, handled below.) The input's
                    // declared `type` field already names the DI / object class.
                    Info.ValueMode = LinkedInput->Input.GetType().IsDataInterface()
                        ? TEXT("data")
                        : TEXT("objectAsset");
                }
                else if (Upstream && MapGetClass && Upstream->IsA(MapGetClass))
                {
                    // Linked-parameter wiring: a Parameter Map Get whose output pin feeding
                    // this override is named for the bound parameter (e.g. "User.Speed").
                    // This is the ONLY "linked" case — SetLinkedParameterValueForFunctionInput
                    // always creates a Map Get node.
                    Info.ValueMode = TEXT("linked");
                    Info.LinkedParameter = UpstreamPin->PinName.ToString();
                }
                else if (Cast<UNiagaraNodeCustomHlsl>(Upstream))
                {
                    // UNiagaraNodeCustomHlsl derives from UNiagaraNodeFunctionCall, so it must
                    // be tested BEFORE the function-call case (matching UpdateValuesFromOverridePin,
                    // which classifies a custom-HLSL override as an inline expression, not a
                    // dynamic input).
                    Info.ValueMode = TEXT("expression");
                }
                else if (const UNiagaraNodeFunctionCall* DynamicInputCall = Cast<UNiagaraNodeFunctionCall>(Upstream))
                {
                    Info.ValueMode = TEXT("dynamicInput");
                    Info.DynamicInputScript = DynamicInputCall->FunctionScript
                        ? DynamicInputCall->FunctionScript->GetPathName()
                        : DynamicInputCall->GetFunctionName();
                }
                else
                {
                    Info.ValueMode = TEXT("connected");
                }
            }
            else
            {
                Info.ValueMode = TEXT("local");
                Info.LiteralValue = Pin->DefaultValue;
            }
            OutByName.Add(Handle.GetName(), Info);
        }
    }

    namespace
    {
        // A static switch lays its option input pins out as [option][output variable] (see
        // UNiagaraNodeStaticSwitch::AllocateDefaultPins), and the Selector pin — present only
        // when the switch is driven by a pin — is created after all of them. Returns the option
        // group Pin belongs to, or INDEX_NONE when the layout does not match the node's declared
        // counts, so an unrecognised shape is never read as a branch.
        int32 SwitchOptionIndexForInputPin(const UNiagaraNodeStaticSwitch& SwitchNode, const UEdGraphPin& Pin)
        {
            static const FName SelectorPinName(TEXT("Selector"));
            const int32 VarCount = SwitchNode.OutputVars.Num();
            const UNiagaraNodeUsageSelector& Selector = SwitchNode;
            const TArray<int32> OptionValues = Selector.GetOptionValues();
            if (VarCount <= 0 || OptionValues.Num() <= 0)
            {
                return INDEX_NONE;
            }

            int32 OptionPinIndex = 0;
            for (const UEdGraphPin* Candidate : SwitchNode.Pins)
            {
                if (!Candidate
                    || Candidate->Direction != EGPD_Input
                    || Candidate->bOrphanedPin
                    || Candidate->PinName == SelectorPinName)
                {
                    continue;
                }
                if (Candidate == &Pin)
                {
                    const int32 OptionIndex = OptionPinIndex / VarCount;
                    return OptionValues.IsValidIndex(OptionIndex) ? OptionIndex : INDEX_NONE;
                }
                ++OptionPinIndex;
            }
            return INDEX_NONE;
        }

        // The caller-facing value that selects one option group, in the same encoding
        // staticSwitchInputs[].value publishes (bool for Bool, the integer for Integer, the
        // branch index for Enum), plus the branch's label.
        TSharedPtr<FJsonValue> SwitchValueForOptionIndex(
            const UNiagaraNodeStaticSwitch& SwitchNode,
            int32 OptionIndex,
            FString& OutLabel)
        {
            OutLabel.Reset();
            switch (SwitchNode.SwitchTypeData.SwitchType)
            {
            case ENiagaraStaticSwitchType::Bool:
            {
                // GetOptionValues orders a bool switch { true, false }.
                const bool bValue = OptionIndex == 0;
                OutLabel = bValue ? TEXT("true") : TEXT("false");
                return MakeShared<FJsonValueBoolean>(bValue);
            }
            case ENiagaraStaticSwitchType::Integer:
            {
                const UNiagaraNodeUsageSelector& Selector = SwitchNode;
                const TArray<int32> OptionValues = Selector.GetOptionValues();
                if (!OptionValues.IsValidIndex(OptionIndex))
                {
                    return nullptr;
                }
                OutLabel = FString::FromInt(OptionValues[OptionIndex]);
                return MakeShared<FJsonValueNumber>(OptionValues[OptionIndex]);
            }
            case ENiagaraStaticSwitchType::Enum:
            {
                TArray<NiagaraStaticSwitch::FEnumSwitchOption> Options;
                NiagaraStaticSwitch::BuildEnumOptions(SwitchNode.SwitchTypeData.Enum, Options);
                if (!Options.IsValidIndex(OptionIndex))
                {
                    return nullptr;
                }
                OutLabel = Options[OptionIndex].DisplayName;
                return MakeShared<FJsonValueNumber>(Options[OptionIndex].Index);
            }
            default:
                return nullptr;
            }
        }

        // Inverse of SwitchValueForOptionIndex: which option group a resolved switch value picks.
        int32 SwitchOptionIndexForValue(const UNiagaraNodeStaticSwitch& SwitchNode, int32 Value)
        {
            switch (SwitchNode.SwitchTypeData.SwitchType)
            {
            case ENiagaraStaticSwitchType::Bool:
                return Value != 0 ? 0 : 1;
            case ENiagaraStaticSwitchType::Integer:
            {
                const UNiagaraNodeUsageSelector& Selector = SwitchNode;
                return Selector.GetOptionValues().IndexOfByKey(Value);
            }
            case ENiagaraStaticSwitchType::Enum:
            {
                TArray<NiagaraStaticSwitch::FEnumSwitchOption> Options;
                NiagaraStaticSwitch::BuildEnumOptions(SwitchNode.SwitchTypeData.Enum, Options);
                return Options.IndexOfByPredicate([Value](const NiagaraStaticSwitch::FEnumSwitchOption& Option)
                {
                    return Option.Index == Value;
                });
            }
            default:
                return INDEX_NONE;
            }
        }

        // Which branch each of the called graph's static switches takes on this placement.
        // Resolution order matches BuildStaticSwitchInputs: the caller pin's stored value when
        // the placed module carries one, otherwise the script's declared default.
        void ResolveSelectedSwitchOptions(
            const UNiagaraNodeFunctionCall& ModuleNode,
            const UNiagaraGraph& CalledGraph,
            TMap<FName, int32>& OutOptionIndexByName)
        {
            TMap<FName, const UEdGraphPin*> PinByName;
            PinByName.Reserve(ModuleNode.Pins.Num());
            for (const UEdGraphPin* Pin : ModuleNode.Pins)
            {
                if (Pin)
                {
                    PinByName.Add(Pin->PinName, Pin);
                }
            }

            for (const TObjectPtr<UEdGraphNode>& GraphNode : CalledGraph.Nodes)
            {
                const UNiagaraNodeStaticSwitch* SwitchNode = Cast<UNiagaraNodeStaticSwitch>(GraphNode);
                if (!SwitchNode || OutOptionIndexByName.Contains(SwitchNode->InputParameterName))
                {
                    continue;
                }

                int32 ResolvedValue = 0;
                bool bResolved = false;
                const UEdGraphPin* const* CallerPinPtr = PinByName.Find(SwitchNode->InputParameterName);
                const UEdGraphPin* CallerPin = CallerPinPtr ? *CallerPinPtr : nullptr;
                if (CallerPin && !CallerPin->DefaultValue.IsEmpty())
                {
                    TSharedPtr<FJsonValue> Decoded;
                    if (NiagaraStaticSwitch::DecodePinDefault(
                            CallerPin->DefaultValue,
                            SwitchNode->SwitchTypeData.SwitchType,
                            SwitchNode->SwitchTypeData.Enum,
                            Decoded)
                        && Decoded.IsValid())
                    {
                        ResolvedValue = Decoded->Type == EJson::Boolean
                            ? (Decoded->AsBool() ? 1 : 0)
                            : static_cast<int32>(Decoded->AsNumber());
                        bResolved = true;
                    }
                }
                if (!bResolved)
                {
                    if (const UNiagaraScriptVariable* SwitchVariable = CalledGraph.GetScriptVariable(SwitchNode->InputParameterName))
                    {
                        ResolvedValue = SwitchVariable->GetStaticSwitchDefaultValue();
                        bResolved = true;
                    }
                }
                if (!bResolved)
                {
                    continue;
                }

                const int32 OptionIndex = SwitchOptionIndexForValue(*SwitchNode, ResolvedValue);
                if (OptionIndex != INDEX_NONE)
                {
                    OutOptionIndexByName.Add(SwitchNode->InputParameterName, OptionIndex);
                }
            }
        }

        // Nodes that carry a value onward unchanged, so a switch behind one still gates the
        // input that feeds it. Anything else counts as a real consumer and makes the input
        // reachable — the walk must never guess that an unknown node is transparent.
        bool IsPassThroughNode(const UEdGraphNode* Node)
        {
            if (!Node)
            {
                return false;
            }
            // UNiagaraNodeConvert has no NIAGARAEDITOR_API, so its class is resolved by name.
            static const UClass* ConvertClass = FindObject<UClass>(nullptr, TEXT("/Script/NiagaraEditor.NiagaraNodeConvert"));
            static const UClass* RerouteClass = FindObject<UClass>(nullptr, TEXT("/Script/NiagaraEditor.NiagaraNodeReroute"));
            return (ConvertClass && Node->IsA(ConvertClass)) || (RerouteClass && Node->IsA(RerouteClass));
        }

        bool FindGateForInput(
            const UNiagaraGraph& CalledGraph,
            const FNiagaraVariable& ModuleInput,
            const TMap<FName, int32>& SelectedOptionByName,
            FModuleInputGate& OutGate)
        {
            OutGate = FModuleInputGate();

            // Every read of the input inside the module graph, whichever node publishes it.
            TArray<const UEdGraphPin*> Frontier;
            for (const TObjectPtr<UEdGraphNode>& GraphNode : CalledGraph.Nodes)
            {
                if (!GraphNode)
                {
                    continue;
                }
                for (const UEdGraphPin* Pin : GraphNode->Pins)
                {
                    if (Pin && Pin->Direction == EGPD_Output && Pin->PinName == ModuleInput.GetName())
                    {
                        Frontier.Add(Pin);
                    }
                }
            }

            bool bFoundGate = false;
            TSet<const UEdGraphPin*> Visited;
            for (int32 Index = 0; Index < Frontier.Num(); ++Index)
            {
                const UEdGraphPin* Source = Frontier[Index];
                if (!Source || Visited.Contains(Source))
                {
                    continue;
                }
                Visited.Add(Source);

                for (const UEdGraphPin* Consumer : Source->LinkedTo)
                {
                    if (!Consumer)
                    {
                        continue;
                    }
                    const UEdGraphNode* ConsumerNode = Consumer->GetOwningNodeUnchecked();
                    if (const UNiagaraNodeStaticSwitch* SwitchNode = Cast<UNiagaraNodeStaticSwitch>(ConsumerNode))
                    {
                        const int32* SelectedOption = SelectedOptionByName.Find(SwitchNode->InputParameterName);
                        const int32 PinOption = SwitchOptionIndexForInputPin(*SwitchNode, *Consumer);
                        if (!SelectedOption || PinOption == INDEX_NONE)
                        {
                            return false;
                        }
                        if (*SelectedOption == PinOption)
                        {
                            return false;
                        }
                        if (!bFoundGate)
                        {
                            FString RequiredLabel;
                            OutGate.SwitchName = SwitchNode->InputParameterName;
                            OutGate.RequiredValue = SwitchValueForOptionIndex(*SwitchNode, PinOption, RequiredLabel);
                            OutGate.CurrentValue = SwitchValueForOptionIndex(*SwitchNode, *SelectedOption, OutGate.BranchTaken);
                            if (!OutGate.RequiredValue.IsValid() || !OutGate.CurrentValue.IsValid())
                            {
                                return false;
                            }
                            bFoundGate = true;
                        }
                        continue;
                    }
                    if (IsPassThroughNode(ConsumerNode))
                    {
                        for (const UEdGraphPin* Onward : ConsumerNode->Pins)
                        {
                            if (Onward && Onward->Direction == EGPD_Output)
                            {
                                Frontier.Add(Onward);
                            }
                        }
                        continue;
                    }
                    // A consumer that is neither a switch branch nor a pass-through reads the
                    // input unconditionally.
                    return false;
                }
            }

            return bFoundGate;
        }
    }

    bool DescribeScriptVariableDefault(
        const UNiagaraScriptVariable& ScriptVariable,
        FModuleInputDefaultInfo& OutInfo)
    {
        OutInfo = FModuleInputDefaultInfo();
        OutInfo.DefaultMode = NiagaraJsonHelpers::EnumToString(ScriptVariable.DefaultMode);

        if (ScriptVariable.DefaultMode == ENiagaraDefaultMode::Binding)
        {
            OutInfo.DefaultBinding = ScriptVariable.DefaultBinding.GetName().ToString();
            return true;
        }
        if (ScriptVariable.DefaultMode != ENiagaraDefaultMode::Value)
        {
            // Custom / FailIfPreviouslyNotSet have no stated value to report; the mode is the
            // whole answer and inventing a number for it would be worse than saying nothing.
            return true;
        }

        // Read the value off Variable, which is the field every engine path that displays or
        // re-serializes a declared default reads (UNiagaraGraph::GetVariable, from which
        // UNiagaraNodeParameterMapGet::CreateDefaultPin builds the default pin string). The
        // parameter panel's edit writes only Variable and mirrors it to those pins, so on a
        // stock module DefaultValueVariant is left zero-filled while Variable carries the
        // authored value; preferring the variant published (0,0,0) for AddVelocityInCone's
        // (1,0,0) Cone Axis. The variant is the fallback for a declaration that allocated it
        // and not Variable, and only at the matching size, since SetData copies the type's
        // full size out of the source buffer.
        FNiagaraVariable DefaultVariable = ScriptVariable.Variable;
        if (!DefaultVariable.IsDataAllocated()
            && ScriptVariable.GetDefaultValueVariant().GetNumBytes() == DefaultVariable.GetSizeInBytes())
        {
            if (const uint8* DefaultData = ScriptVariable.GetDefaultValueData())
            {
                DefaultVariable.SetData(DefaultData);
            }
        }
        // TryGetPinDefaultValueFromNiagaraVariable silently substitutes the *type's* zero
        // default for an unallocated variable, which would publish a fabricated value under a
        // "Value" mode, so only encode when real data is there.
        if (!DefaultVariable.IsDataAllocated())
        {
            return true;
        }
        GetDefault<UEdGraphSchema_Niagara>()->TryGetPinDefaultValueFromNiagaraVariable(
            DefaultVariable, OutInfo.DefaultValue);
        return true;
    }

    bool ResolveModuleInputDefault(
        const UNiagaraGraph* ModuleGraph,
        const FNiagaraVariable& ModuleInput,
        FModuleInputDefaultInfo& OutInfo)
    {
        OutInfo = FModuleInputDefaultInfo();
        if (!ModuleGraph)
        {
            return false;
        }
        const TObjectPtr<UNiagaraScriptVariable>* Found = ModuleGraph->GetAllMetaData().Find(ModuleInput);
        const UNiagaraScriptVariable* ScriptVariable = Found ? Found->Get() : nullptr;
        if (!ScriptVariable)
        {
            // The metadata map is keyed on name AND type, and the enumerated stack input's type
            // can be the resolved rather than the declared one. Module-namespace inputs are
            // unique by name within a module graph, so the by-name lookup is a safe fallback.
            ScriptVariable = ModuleGraph->GetScriptVariable(ModuleInput.GetName());
        }
        if (!ScriptVariable)
        {
            return false;
        }
        return DescribeScriptVariableDefault(*ScriptVariable, OutInfo);
    }

    void ClassifyModuleInputReachability(
        const UNiagaraNodeFunctionCall& ModuleNode,
        const TArray<FNiagaraVariable>& DeclaredInputs,
        TMap<FName, FModuleInputGate>& OutGatedByName)
    {
        OutGatedByName.Empty();
        const UNiagaraGraph* CalledGraph = const_cast<UNiagaraNodeFunctionCall&>(ModuleNode).GetCalledGraph();
        if (!CalledGraph)
        {
            return;
        }

        TMap<FName, int32> SelectedOptionByName;
        ResolveSelectedSwitchOptions(ModuleNode, *CalledGraph, SelectedOptionByName);
        if (SelectedOptionByName.IsEmpty())
        {
            return;
        }

        for (const FNiagaraVariable& Input : DeclaredInputs)
        {
            FModuleInputGate Gate;
            if (FindGateForInput(*CalledGraph, Input, SelectedOptionByName, Gate))
            {
                OutGatedByName.Add(FNiagaraParameterHandle(Input.GetName()).GetName(), Gate);
            }
        }
    }

    bool FindModuleInputGate(
        const UNiagaraNodeFunctionCall& ModuleNode,
        FName ShortInputName,
        FModuleInputGate& OutGate)
    {
        OutGate = FModuleInputGate();
        TArray<FNiagaraVariable> DeclaredInputs;
        EnumerateModuleStackInputs(ModuleNode, DeclaredInputs);
        DeclaredInputs.RemoveAll([ShortInputName](const FNiagaraVariable& Input)
        {
            return FNiagaraParameterHandle(Input.GetName()).GetName() != ShortInputName;
        });

        TMap<FName, FModuleInputGate> Gated;
        ClassifyModuleInputReachability(ModuleNode, DeclaredInputs, Gated);
        if (const FModuleInputGate* Found = Gated.Find(ShortInputName))
        {
            OutGate = *Found;
            return true;
        }
        return false;
    }

    TSharedPtr<FJsonObject> MakeGatedByJson(const FModuleInputGate& Gate)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("switch"), Gate.SwitchName.ToString());
        if (Gate.CurrentValue.IsValid())
        {
            Obj->SetField(TEXT("value"), Gate.CurrentValue);
        }
        if (Gate.RequiredValue.IsValid())
        {
            Obj->SetField(TEXT("requiredValue"), Gate.RequiredValue);
        }
        Obj->SetStringField(TEXT("branchTaken"), Gate.BranchTaken);
        return Obj;
    }
}

namespace NiagaraStaticSwitch
{
    bool FindByName(const UNiagaraGraph* CalledGraph, FName InputName, const UNiagaraNodeStaticSwitch*& OutDecl)
    {
        OutDecl = nullptr;
        if (!CalledGraph)
        {
            return false;
        }
        for (const TObjectPtr<UEdGraphNode>& GraphNode : CalledGraph->Nodes)
        {
            const UNiagaraNodeStaticSwitch* Candidate = Cast<UNiagaraNodeStaticSwitch>(GraphNode);
            if (Candidate && Candidate->InputParameterName == InputName)
            {
                OutDecl = Candidate;
                return true;
            }
        }
        return false;
    }

    namespace
    {
        // Spaces and case are presentation, not identity: the enum editor shows "Random Uniform"
        // and a caller may reasonably type "randomuniform" or "Random_Uniform" for it.
        FString CollapseLabel(const FString& In)
        {
            FString Out = In;
            Out.ReplaceInline(TEXT(" "), TEXT(""), ESearchCase::CaseSensitive);
            Out.ReplaceInline(TEXT("_"), TEXT(""), ESearchCase::CaseSensitive);
            return Out.ToLower();
        }

        bool IsIntegerLiteral(const FString& In)
        {
            if (In.IsEmpty())
            {
                return false;
            }
            const int32 Start = (In[0] == TEXT('-') || In[0] == TEXT('+')) ? 1 : 0;
            if (Start >= In.Len())
            {
                return false;
            }
            for (int32 Pos = Start; Pos < In.Len(); ++Pos)
            {
                if (!FChar::IsDigit(In[Pos]))
                {
                    return false;
                }
            }
            return true;
        }

        // The whole branch table, inlined into the rejection message. A caller who guessed wrong
        // gets the mapping from the failure itself rather than having to probe one integer at a
        // time; enum switches carry a handful of branches, so the message stays readable.
        FString DescribeOptions(const TArray<FEnumSwitchOption>& Options)
        {
            TArray<FString> Parts;
            Parts.Reserve(Options.Num());
            for (const FEnumSwitchOption& Option : Options)
            {
                Parts.Add(Option.DisplayName.Equals(Option.Name, ESearchCase::CaseSensitive)
                    ? FString::Printf(TEXT("%s (index %d)"), *Option.Name, Option.Index)
                    : FString::Printf(TEXT("%s / %s (index %d)"), *Option.DisplayName, *Option.Name, Option.Index));
            }
            return FString::Join(Parts, TEXT(", "));
        }
    }

    void BuildEnumOptions(const UEnum* Enum, TArray<FEnumSwitchOption>& OutOptions)
    {
        OutOptions.Reset();
        if (!Enum)
        {
            return;
        }
        const int32 NumEntries = Enum->NumEnums();
        for (int32 Index = 0; Index < NumEntries; ++Index)
        {
            const FString Name = Enum->GetNameStringByIndex(Index);
            if (Name.IsEmpty() || BlueprintEnumHelpers::IsEnumMaxEntryName(Name))
            {
                continue;
            }
            // Mirrors FNiagaraEnumIndexVisibilityCache::GetVisibility, which is what
            // UNiagaraNodeStaticSwitch::GetOptionValues filters on: an entry the editor hides is
            // not one of the switch's branches, so it is not selectable here either.
            if (Enum->HasMetaData(TEXT("Hidden"), Index) || Enum->HasMetaData(TEXT("Spacer"), Index))
            {
                continue;
            }
            FEnumSwitchOption& Option = OutOptions.AddDefaulted_GetRef();
            Option.Index = Index;
            Option.Name = Name;
            Option.DisplayName = Enum->GetDisplayNameTextByIndex(Index).ToString();
        }
    }

    TArray<TSharedPtr<FJsonValue>> MakeEnumOptionsJson(const UEnum* Enum)
    {
        TArray<FEnumSwitchOption> Options;
        BuildEnumOptions(Enum, Options);

        TArray<TSharedPtr<FJsonValue>> Values;
        Values.Reserve(Options.Num());
        for (const FEnumSwitchOption& Option : Options)
        {
            TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
            Entry->SetNumberField(TEXT("index"), Option.Index);
            Entry->SetStringField(TEXT("name"), Option.Name);
            Entry->SetStringField(TEXT("displayName"), Option.DisplayName);
            Values.Add(MakeShared<FJsonValueObject>(Entry));
        }
        return Values;
    }

    bool ResolveEnumOption(
        const UEnum* Enum,
        const TSharedPtr<FJsonValue>& Json,
        FEnumSwitchOption& OutOption,
        FString& OutError)
    {
        OutOption = FEnumSwitchOption();
        if (!Enum)
        {
            OutError = TEXT("Enum static switch has no Enum class set.");
            return false;
        }

        TArray<FEnumSwitchOption> Options;
        BuildEnumOptions(Enum, Options);
        if (Options.Num() == 0)
        {
            OutError = FString::Printf(TEXT("Enum '%s' offers no selectable static switch branches."), *Enum->GetName());
            return false;
        }

        bool bHaveIndex = false;
        int32 RequestedIndex = INDEX_NONE;
        FString Literal;
        if (Json.IsValid() && Json->Type == EJson::Number)
        {
            RequestedIndex = static_cast<int32>(Json->AsNumber());
            bHaveIndex = true;
        }
        else if (Json.IsValid() && Json->Type == EJson::String)
        {
            Literal = Json->AsString().TrimStartAndEnd();
            int32 Parsed = 0;
            if (IsIntegerLiteral(Literal) && LexTryParseString(Parsed, *Literal))
            {
                RequestedIndex = Parsed;
                bHaveIndex = true;
            }
        }
        else
        {
            // Anything else used to fall through to index 0 and write a branch nobody asked for.
            OutError = FString::Printf(
                TEXT("Enum static switch value must be a branch index or a name; accepted values on '%s': %s."),
                *Enum->GetName(), *DescribeOptions(Options));
            return false;
        }

        if (bHaveIndex)
        {
            // A number stays the branch index, which is what Niagara's own
            // FNiagaraEditorUtilities::ResolveConstantValue produces from the pin default. The
            // range check is what is new: an index outside the table used to be written anyway
            // and then clamped to branch 0 by the compiler, with nothing reporting it.
            const FEnumSwitchOption* Found = Options.FindByPredicate(
                [RequestedIndex](const FEnumSwitchOption& Option) { return Option.Index == RequestedIndex; });
            if (Found)
            {
                OutOption = *Found;
                return true;
            }
            OutError = FString::Printf(
                TEXT("Enum branch index %d is not selectable on '%s'; accepted values: %s."),
                RequestedIndex, *Enum->GetName(), *DescribeOptions(Options));
            return false;
        }

        // Authored entry name, fully qualified name, redirect, or display name — the same
        // resolution the Blueprint enum-pin path already does. It answers with the enum value,
        // which maps back to the branch index the switch selects on.
        int64 ResolvedValue = INDEX_NONE;
        if (BlueprintEnumHelpers::TryResolveEnumLiteralToValue(Enum, Literal, ResolvedValue))
        {
            const int32 ValueIndex = Enum->GetIndexByValue(ResolvedValue);
            const FEnumSwitchOption* Found = Options.FindByPredicate(
                [ValueIndex](const FEnumSwitchOption& Option) { return Option.Index == ValueIndex; });
            if (Found)
            {
                OutOption = *Found;
                return true;
            }
        }

        const FString Collapsed = CollapseLabel(Literal);
        for (const FEnumSwitchOption& Option : Options)
        {
            if (CollapseLabel(Option.DisplayName) == Collapsed || CollapseLabel(Option.Name) == Collapsed)
            {
                OutOption = Option;
                return true;
            }
        }

        OutError = FString::Printf(
            TEXT("Enum value '%s' not found on '%s'; accepted values: %s."),
            *Literal, *Enum->GetName(), *DescribeOptions(Options));
        return false;
    }

    bool ValidateIntegerOption(
        const UNiagaraNodeStaticSwitch& SwitchDecl,
        int32 Value,
        FString& OutError)
    {
        OutError.Reset();
        // Call through the public base virtual so Niagara remains the source of truth for both
        // ordinary integer switches and integer switches whose inline enum metadata sets the
        // branch count.
        const UNiagaraNodeUsageSelector& Selector = SwitchDecl;
        const TArray<int32> Options = Selector.GetOptionValues();
        if (Options.Contains(Value))
        {
            return true;
        }

        if (Options.IsEmpty())
        {
            OutError = FString::Printf(
                TEXT("Integer static switch value %d is invalid; the switch declares no selectable options."),
                Value);
            return false;
        }

        int32 MinValue = Options[0];
        int32 MaxValue = Options[0];
        for (const int32 Option : Options)
        {
            MinValue = FMath::Min(MinValue, Option);
            MaxValue = FMath::Max(MaxValue, Option);
        }
        OutError = FString::Printf(
            TEXT("Integer static switch value %d is outside the valid range %d..%d."),
            Value, MinValue, MaxValue);
        return false;
    }

    bool DecodePinDefault(
        const FString& PinDefault,
        ENiagaraStaticSwitchType Type,
        const UEnum* Enum,
        TSharedPtr<FJsonValue>& OutValue,
        FString* OutError)
    {
        switch (Type)
        {
        case ENiagaraStaticSwitchType::Bool:
            OutValue = MakeShared<FJsonValueBoolean>(PinDefault.Equals(TEXT("true"), ESearchCase::IgnoreCase));
            return true;
        case ENiagaraStaticSwitchType::Integer:
            OutValue = MakeShared<FJsonValueNumber>(static_cast<double>(FCString::Atoi(*PinDefault)));
            return true;
        case ENiagaraStaticSwitchType::Enum:
        {
            // An unresolvable stored name used to become branch 0 and be published as if it
            // were the authored value, which no reader can tell apart from a real branch-0
            // override. Report the miss so the caller can decide what to say about the pin.
            if (!Enum)
            {
                if (OutError)
                {
                    *OutError = FString::Printf(
                        TEXT("Enum static switch has no enum class; stored value '%s' cannot be decoded."),
                        *PinDefault);
                }
                return false;
            }
            const FString FullName = Enum->GenerateFullEnumName(*PinDefault);
            const int32 Found = Enum->GetIndexByName(FName(*FullName));
            if (Found == INDEX_NONE)
            {
                if (OutError)
                {
                    TArray<FEnumSwitchOption> Options;
                    BuildEnumOptions(Enum, Options);
                    *OutError = FString::Printf(
                        TEXT("Stored enum value '%s' is not an entry of '%s'; accepted values: %s."),
                        *PinDefault, *Enum->GetName(), *DescribeOptions(Options));
                }
                return false;
            }
            OutValue = MakeShared<FJsonValueNumber>(static_cast<double>(Found));
            return true;
        }
        }
        if (OutError)
        {
            *OutError = TEXT("Unsupported static-switch type.");
        }
        return false;
    }

    bool EncodePinDefault(
        const TSharedPtr<FJsonValue>& Json,
        ENiagaraStaticSwitchType Type,
        const UEnum* Enum,
        FString& OutPinDefault,
        FString& OutError,
        FEnumSwitchOption* OutEnumOption)
    {
        switch (Type)
        {
        case ENiagaraStaticSwitchType::Bool:
        {
            bool bBool = false;
            if (Json.IsValid() && Json->Type == EJson::Boolean)
            {
                bBool = Json->AsBool();
            }
            else if (Json.IsValid() && Json->Type == EJson::Number)
            {
                bBool = Json->AsNumber() != 0.0;
            }
            else if (Json.IsValid() && Json->Type == EJson::String)
            {
                bBool = Json->AsString().Equals(TEXT("true"), ESearchCase::IgnoreCase);
            }
            OutPinDefault = bBool ? TEXT("true") : TEXT("false");
            return true;
        }
        case ENiagaraStaticSwitchType::Integer:
        {
            int32 IntValue = 0;
            if (Json.IsValid() && Json->Type == EJson::Boolean)
            {
                IntValue = Json->AsBool() ? 1 : 0;
            }
            else if (Json.IsValid() && Json->Type == EJson::Number)
            {
                IntValue = static_cast<int32>(Json->AsNumber());
            }
            else if (Json.IsValid() && Json->Type == EJson::String)
            {
                IntValue = FCString::Atoi(*Json->AsString());
            }
            OutPinDefault = FString::FromInt(IntValue);
            return true;
        }
        case ENiagaraStaticSwitchType::Enum:
        {
            FEnumSwitchOption Option;
            if (!ResolveEnumOption(Enum, Json, Option, OutError))
            {
                return false;
            }
            if (OutEnumOption)
            {
                *OutEnumOption = Option;
            }
            // The caller pin stores the short, un-prefixed AUTHORED entry name — NewEnumeratorN on
            // a user-defined enum, not the label the editor shows. That is what Niagara's own
            // FNiagaraEditorUtilities::ResolveConstantValue reads back through GenerateFullEnumName
            // + GetIndexByName, so a display name written here would not resolve to any branch.
            OutPinDefault = Option.Name;
            return true;
        }
        }
        OutError = TEXT("Unsupported static switch type.");
        return false;
    }
}
