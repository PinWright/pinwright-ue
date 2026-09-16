// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Tests/Niagara/TestNIRFixtures.h"

#include "Handlers/Niagara/NiagaraSystemViewModelCache.h"
#include "Tests/Assets/NiagaraEditTestUtils.h"
#include "Handlers/Niagara/NiagaraInstanceUtils.h"

#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_Niagara.h"
#include "Compat/EngineVersionCompat.h"
#include "Misc/Guid.h"
#include "NiagaraEmitter.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraGraph.h"
#include "NiagaraNodeFunctionCall.h"
#include "NiagaraNodeOutput.h"
#include "NiagaraScript.h"
#include "NiagaraScriptSource.h"
#include "ViewModels/Stack/NiagaraStackGraphUtilities.h"
#include "NiagaraSystem.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#include "ViewModels/NiagaraSystemViewModel.h"

namespace NIRTestFixtures
{
    namespace
    {
        // Return the emitter graph for the first emitter on System, or nullptr when the
        // system has no emitter handle yet. Used by particle/emitter-scope module adds.
        UNiagaraGraph* GetFirstEmitterGraph(UNiagaraSystem* System)
        {
            if (!System)
            {
                return nullptr;
            }
            for (const FNiagaraEmitterHandle& Handle : System->GetEmitterHandles())
            {
                if (FVersionedNiagaraEmitterData* EmitterData = Handle.GetEmitterData())
                {
                    if (UNiagaraScriptSourceBase* SourceBase = EmitterData->GraphSource)
                    {
                        if (UNiagaraScriptSource* Source = Cast<UNiagaraScriptSource>(SourceBase))
                        {
                            return Source->NodeGraph;
                        }
                    }
                }
            }
            return nullptr;
        }

        // System-scope (SystemSpawn / SystemUpdate) modules land in the system spawn
        // script's graph, mirroring AppendStack's UsageScopeOf in NIRDecompiler.
        UNiagaraGraph* GetSystemGraph(UNiagaraSystem* System)
        {
            if (!System || !System->GetSystemSpawnScript())
            {
                return nullptr;
            }
            if (UNiagaraScriptSourceBase* SourceBase = System->GetSystemSpawnScript()->GetLatestSource())
            {
                if (UNiagaraScriptSource* Source = Cast<UNiagaraScriptSource>(SourceBase))
                {
                    return Source->NodeGraph;
                }
            }
            return nullptr;
        }

        UNiagaraGraph* GetGraphForUsage(UNiagaraSystem* System, ENiagaraScriptUsage Usage)
        {
            switch (Usage)
            {
            case ENiagaraScriptUsage::SystemSpawnScript:
            case ENiagaraScriptUsage::SystemUpdateScript:
                return GetSystemGraph(System);
            default:
                return GetFirstEmitterGraph(System);
            }
        }
    }

    UNiagaraSystem* BuildEmptySystemWithEmitter(FName SystemName)
    {
        FString IgnoredObjectPath;
        UNiagaraSystem* System = NiagaraEditTestUtils::NewTransientSystem(IgnoredObjectPath);
        if (!System)
        {
            return nullptr;
        }

        // NewTransientSystem already emptied the duplicated source's emitter handles.
        // Duplicate one emitter from the same fixture asset to seed a real authored
        // emitter graph the stack-edit utilities can land modules in.
        FString EmitterObjectPath;
        UNiagaraEmitter* Emitter = NiagaraEditTestUtils::NewTransientEmitter(EmitterObjectPath);
        if (!Emitter)
        {
            DestroyFixture(System);
            return nullptr;
        }

        // SystemName names the emitter handle within the system. Asset-level uniqueness
        // comes from MakeAssetName's GUID suffix, so duplicate SystemName values across
        // fixtures are safe.
        System->AddEmitterHandle(*Emitter, SystemName, Emitter->GetExposedVersion().VersionGuid);

        // Touch the SVM so RefreshAll runs once with bIsForDataProcessingOnly=true. The
        // override-pin and override-node scaffolding the v1b walker reads is only
        // populated after the SVM has primed the stack view models. Subsequent fixture
        // helpers (AddModuleToStack / SetModuleInput*) reuse the same cached SVM.
        PinWrightNiagara::AcquireSystemViewModel(*System);
        return System;
    }

    UNiagaraNodeFunctionCall* AddModuleToStack(
        UNiagaraSystem* System,
        ENiagaraScriptUsage Usage,
        UNiagaraScript* ModuleScript)
    {
        if (!System || !ModuleScript)
        {
            return nullptr;
        }

        UNiagaraGraph* Graph = GetGraphForUsage(System, Usage);
        if (!Graph)
        {
            return nullptr;
        }

        UNiagaraNodeOutput* OutputNode = Graph->FindEquivalentOutputNode(Usage, FGuid());
        if (!OutputNode)
        {
            return nullptr;
        }

        // INDEX_NONE appends at the stack's tail — same convention the
        // niagara.edit AddModule handler uses when ToIndex is omitted.
        return FNiagaraStackGraphUtilities::AddScriptModuleToStack(
            ModuleScript,
            *OutputNode,
            INDEX_NONE,
            ModuleScript->GetName());
    }

    void SetModuleInputLiteral(
        UNiagaraNodeFunctionCall* ModuleNode,
        FName InputName,
        FNiagaraVariable Value)
    {
        if (!ModuleNode || !Value.IsValid())
        {
            return;
        }

        const FNiagaraParameterHandle InputHandle = FNiagaraParameterHandle::CreateModuleParameterHandle(InputName);
        const FNiagaraParameterHandle AliasedInputHandle =
            FNiagaraParameterHandle::CreateAliasedModuleParameterHandle(InputHandle, ModuleNode);

        UEdGraphPin& OverridePin = FNiagaraStackGraphUtilities::GetOrCreateStackFunctionInputOverridePin(
            *ModuleNode,
            AliasedInputHandle,
            Value.GetType(),
            FGuid(),
            FGuid());
        OverridePin.Modify();

        // FNiagaraVariable stores the literal as raw bytes; the schema's TrySetDefaultValue
        // expects a serialised pin-default string. FNiagaraTypeDefinition::ToString reflects
        // the per-type Niagara struct's serialisation (Vector3.X=..., etc.) — same shape the
        // niagara.edit SetModuleInput handler ultimately writes for struct payloads.
        const FString DefaultValue = Value.IsDataAllocated()
            ? Value.GetType().ToString(Value.GetData())
            : FString();
        if (!DefaultValue.IsEmpty())
        {
            GetDefault<UEdGraphSchema_Niagara>()->TrySetDefaultValue(OverridePin, DefaultValue, true);
        }
    }

    void SetModuleInputLinkedParam(
        UNiagaraNodeFunctionCall* ModuleNode,
        FName InputName,
        FName ParameterHandle)
    {
        if (!ModuleNode || InputName.IsNone() || ParameterHandle.IsNone())
        {
            return;
        }

        const FNiagaraParameterHandle InputHandle = FNiagaraParameterHandle::CreateModuleParameterHandle(InputName);
        const FNiagaraParameterHandle AliasedInputHandle =
            FNiagaraParameterHandle::CreateAliasedModuleParameterHandle(InputHandle, ModuleNode);

        // Default the link type to float; callers asserting a specific type can rebuild
        // the link with FNiagaraStackGraphUtilities::SetLinkedParameterValueForFunctionInput
        // after this returns. Most NIR tests only need the override-node + UNiagaraNodeInput
        // structure for the walker to classify the link as "$Namespace.Name".
        const FNiagaraTypeDefinition LinkType = FNiagaraTypeDefinition::GetFloatDef();

        UEdGraphPin& OverridePin = FNiagaraStackGraphUtilities::GetOrCreateStackFunctionInputOverridePin(
            *ModuleNode,
            AliasedInputHandle,
            LinkType,
            FGuid(),
            FGuid());
        OverridePin.Modify();

        // Insert a UNiagaraNodeInput configured for the linked-parameter handle and wire
        // its output pin to the override pin. The override-node walker classifies an
        // override-pin link to a UNiagaraNodeInput as the linked-parameter case.
        // The KnownParameters set is consulted only to skip an "is this parameter already
        // present?" early-return — an empty set is the path AddLinkedInput-style flow uses.
        //
        // SetLinkedParameterValueForFunctionInput (FNiagaraVariableBase, TSet<FNiagaraVariableBase>)
        // was introduced in UE 5.6, replacing SetLinkedValueHandleForFunctionInput
        // (FNiagaraParameterHandle, TSet<FNiagaraVariable>) that exists on UE 5.4 and 5.5.
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 6, 0)
        const FNiagaraVariableBase LinkedParameter(LinkType, ParameterHandle);
        const TSet<FNiagaraVariableBase> KnownParameters;
        FNiagaraStackGraphUtilities::SetLinkedParameterValueForFunctionInput(
            OverridePin,
            LinkedParameter,
            KnownParameters,
            ENiagaraDefaultMode::FailIfPreviouslyNotSet,
            FGuid());
#else
        const FNiagaraParameterHandle LinkedParameterHandle(ParameterHandle);
        const TSet<FNiagaraVariable> KnownParameters;
        FNiagaraStackGraphUtilities::SetLinkedValueHandleForFunctionInput(
            OverridePin,
            LinkedParameterHandle,
            KnownParameters,
            ENiagaraDefaultMode::FailIfPreviouslyNotSet,
            FGuid());
#endif
    }

    UNiagaraNodeFunctionCall* SetModuleInputDynamicInput(
        UNiagaraNodeFunctionCall* ModuleNode,
        FName InputName,
        UNiagaraScript* DynamicInputScript)
    {
        if (!ModuleNode || InputName.IsNone() || !DynamicInputScript)
        {
            return nullptr;
        }

        const FNiagaraParameterHandle InputHandle = FNiagaraParameterHandle::CreateModuleParameterHandle(InputName);
        const FNiagaraParameterHandle AliasedInputHandle =
            FNiagaraParameterHandle::CreateAliasedModuleParameterHandle(InputHandle, ModuleNode);

        const FNiagaraTypeDefinition InputType = FNiagaraTypeDefinition::GetFloatDef();

        UEdGraphPin& OverridePin = FNiagaraStackGraphUtilities::GetOrCreateStackFunctionInputOverridePin(
            *ModuleNode,
            AliasedInputHandle,
            InputType,
            FGuid(),
            FGuid());
        OverridePin.Modify();

        // Insert a dynamic-input function-call node on the override chain. The new
        // UNiagaraNodeFunctionCall's FunctionScript is DynamicInputScript and its
        // single output pin is wired to OverridePin — that is what the v1b walker
        // recognises as "dynamic ModuleName { ... }".
        UNiagaraNodeFunctionCall* DynamicInputNode = nullptr;
        FNiagaraStackGraphUtilities::SetDynamicInputForFunctionInput(
            OverridePin,
            DynamicInputScript,
            DynamicInputNode,
            FGuid(),
            FString(),
            FGuid());
        return DynamicInputNode;
    }

    void DestroyFixture(UNiagaraSystem* System)
    {
        if (!System)
        {
            return;
        }
        // Kill any running preview instances first; otherwise their SimCaches retain a
        // back-pointer to System and subsequent fixture SVM construction can re-use
        // stale state from a torn-down system.
        PinWrightNiagara::KillSystemInstances(*System);
        PinWrightNiagara::ReleaseSystemViewModel(*System);

        // NewTransientEmitter AddToRoots each emitter; without matching RemoveFromRoots
        // they would leak until module shutdown. Walk the handles before unrooting the
        // system itself so the handle array is still valid.
        for (const FNiagaraEmitterHandle& Handle : System->GetEmitterHandles())
        {
            if (UNiagaraEmitter* Emitter = Handle.GetInstance().Emitter.Get())
            {
                Emitter->RemoveFromRoot();
            }
        }
        System->RemoveFromRoot();
    }
}
