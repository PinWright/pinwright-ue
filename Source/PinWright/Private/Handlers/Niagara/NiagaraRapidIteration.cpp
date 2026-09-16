// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/Niagara/NiagaraRapidIteration.h"

#include "EdGraphSchema_Niagara.h"
#include "NiagaraCommon.h"
#include "NiagaraEmitter.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraParameterStore.h"
#include "NiagaraScript.h"
#include "NiagaraSystem.h"

namespace PinWrightNiagara
{
    namespace
    {
        void AddStore(TArray<FRapidIterationStoreRef>& OutStores, UNiagaraScript* Script, const TCHAR* Scope)
        {
            if (Script)
            {
                OutStores.Add(FRapidIterationStoreRef{ Script, Scope });
            }
        }

        void AddEmitterDataStores(TArray<FRapidIterationStoreRef>& OutStores, FVersionedNiagaraEmitterData* EmitterData)
        {
            if (!EmitterData)
            {
                return;
            }
            AddStore(OutStores, EmitterData->EmitterSpawnScriptProps.Script, TEXT("emitterSpawnRapidIteration"));
            AddStore(OutStores, EmitterData->EmitterUpdateScriptProps.Script, TEXT("emitterUpdateRapidIteration"));
            AddStore(OutStores, EmitterData->SpawnScriptProps.Script, TEXT("spawnRapidIteration"));
            AddStore(OutStores, EmitterData->UpdateScriptProps.Script, TEXT("updateRapidIteration"));
            AddStore(OutStores, EmitterData->GetGPUComputeScript(), TEXT("gpuComputeRapidIteration"));
        }

        // Read one parameter's value out of a store into user-facing bytes. Uses
        // CopyParameterData rather than a raw memcpy off the offset so an LWC type
        // (FVector / FQuat / ...) comes back in the same layout SetParameterData expects.
        bool ReadParameterBytes(
            const FNiagaraParameterStore& Store,
            const FNiagaraVariable& Variable,
            TArray<uint8>& OutData)
        {
            const int32 Size = Variable.GetSizeInBytes();
            if (Size <= 0 || Store.IndexOf(Variable) == INDEX_NONE)
            {
                return false;
            }
            OutData.SetNumUninitialized(Size);
            return Store.CopyParameterData(Variable, OutData.GetData());
        }

        FString DescribeValue(const FNiagaraTypeDefinition& Type, const uint8* Data)
        {
            if (!Data || !Type.IsValid())
            {
                return FString();
            }
            FNiagaraVariable Value(Type, NAME_None);
            Value.SetData(Data);
            FString Text;
            if (GetDefault<UEdGraphSchema_Niagara>()->TryGetPinDefaultValueFromNiagaraVariable(Value, Text))
            {
                return Text;
            }
            return FString();
        }
    }

    void CollectRapidIterationStores(
        UNiagaraSystem* System,
        UNiagaraEmitter* Emitter,
        TArray<FRapidIterationStoreRef>& OutStores)
    {
        if (System)
        {
            // Emitter-stage stores first: PrepareRapidIterationParameters copies them over the
            // system-script mirrors, so they are the values that outlive a compile.
            for (const FNiagaraEmitterHandle& Handle : System->GetEmitterHandles())
            {
                AddEmitterDataStores(OutStores, Handle.GetEmitterData());
            }
            AddStore(OutStores, System->GetSystemSpawnScript(), TEXT("systemSpawnRapidIteration"));
            AddStore(OutStores, System->GetSystemUpdateScript(), TEXT("systemUpdateRapidIteration"));
            return;
        }

        if (Emitter)
        {
            AddEmitterDataStores(OutStores, Emitter->GetLatestEmitterData());
        }
    }

    FName MakeRapidIterationConstantName(
        FName AliasedInputName,
        const FString& UniqueEmitterName,
        ENiagaraScriptUsage OwningUsage)
    {
        // A module in a system stage has no emitter segment; every other stage carries one, even
        // when the module is reached through the system's own graph. Same two-way split as the
        // engine's FNiagaraStackGraphUtilities::CreateRapidIterationParameter, including its
        // handling of an empty emitter name: the non-system branch passes the string through
        // rather than substituting nullptr, which would drop the separator the engine emits.
        const bool bSystemStage = OwningUsage == ENiagaraScriptUsage::SystemSpawnScript
            || OwningUsage == ENiagaraScriptUsage::SystemUpdateScript;
        const TCHAR* EmitterSegment = bSystemStage ? nullptr : *UniqueEmitterName;
        return FName(*FNiagaraUtilities::CreateRapidIterationConstantName(AliasedInputName, EmitterSegment, OwningUsage));
    }

    void WriteThroughModuleInputConstant(
        UNiagaraSystem* System,
        UNiagaraEmitter* Emitter,
        FName ConstantName,
        const FNiagaraTypeDefinition& InputType,
        const uint8* ValueData,
        FRapidIterationWriteThrough& OutResult)
    {
        OutResult.ParameterName = ConstantName.ToString();
        if (ConstantName.IsNone() || !ValueData || !InputType.IsValid())
        {
            return;
        }

        TArray<FRapidIterationStoreRef> Stores;
        CollectRapidIterationStores(System, Emitter, Stores);

        for (const FRapidIterationStoreRef& StoreRef : Stores)
        {
            FNiagaraParameterStore& Store = StoreRef.Script->RapidIterationParameters;
            const FNiagaraVariableWithOffset* Existing = nullptr;
            for (const FNiagaraVariableWithOffset& Candidate : Store.ReadParameterVariables())
            {
                if (Candidate.GetName() == ConstantName)
                {
                    Existing = &Candidate;
                    break;
                }
            }
            if (!Existing)
            {
                continue;
            }

            OutResult.bShadowed = true;
            const FNiagaraVariable StoredVariable(Existing->GetType(), Existing->GetName());
            if (Existing->GetType() != InputType)
            {
                // The store disagrees with the pin about the input's type, so writing the pin's
                // bytes would reinterpret them. Report the shadow and leave it standing rather
                // than corrupting the entry.
                OutResult.TypeMismatchScopes.AddUnique(StoreRef.Scope);
                continue;
            }

            TArray<uint8> PreviousData;
            if (OutResult.PreviousValue.IsEmpty() && ReadParameterBytes(Store, StoredVariable, PreviousData))
            {
                OutResult.PreviousValue = DescribeValue(InputType, PreviousData.GetData());
            }

            StoreRef.Script->Modify();
            Store.SetParameterData(ValueData, StoredVariable, /*bAdd=*/false);
            OutResult.UpdatedScopes.AddUnique(StoreRef.Scope);
            OutResult.Action = TEXT("updated");
        }
    }

    void RemoveModuleInputConstant(
        UNiagaraSystem* System,
        UNiagaraEmitter* Emitter,
        FName ConstantName,
        FRapidIterationWriteThrough& OutResult)
    {
        OutResult.ParameterName = ConstantName.ToString();
        if (ConstantName.IsNone())
        {
            return;
        }

        TArray<FRapidIterationStoreRef> Stores;
        CollectRapidIterationStores(System, Emitter, Stores);

        for (const FRapidIterationStoreRef& StoreRef : Stores)
        {
            FNiagaraParameterStore& Store = StoreRef.Script->RapidIterationParameters;
            const FNiagaraVariableWithOffset* Existing = nullptr;
            for (const FNiagaraVariableWithOffset& Candidate : Store.ReadParameterVariables())
            {
                if (Candidate.GetName() == ConstantName)
                {
                    Existing = &Candidate;
                    break;
                }
            }
            if (!Existing)
            {
                continue;
            }

            OutResult.bShadowed = true;
            // Copy off the view before the removal reshuffles the store's offsets.
            const FNiagaraVariable StoredVariable(Existing->GetType(), Existing->GetName());
            TArray<uint8> PreviousData;
            if (OutResult.PreviousValue.IsEmpty() && ReadParameterBytes(Store, StoredVariable, PreviousData))
            {
                OutResult.PreviousValue = DescribeValue(StoredVariable.GetType(), PreviousData.GetData());
            }

            StoreRef.Script->Modify();
            if (Store.RemoveParameter(StoredVariable))
            {
                OutResult.UpdatedScopes.AddUnique(StoreRef.Scope);
                OutResult.Action = TEXT("removed");
            }
        }
    }

    void FRapidIterationValueSnapshot::Capture(UNiagaraSystem& System)
    {
        Entries.Reset();

        TArray<FRapidIterationStoreRef> Stores;
        CollectRapidIterationStores(&System, nullptr, Stores);
        for (const FRapidIterationStoreRef& StoreRef : Stores)
        {
            const FNiagaraParameterStore& Store = StoreRef.Script->RapidIterationParameters;
            for (const FNiagaraVariableWithOffset& Variable : Store.ReadParameterVariables())
            {
                if (Variable.IsDataInterface() || Variable.IsUObject())
                {
                    continue;
                }
                FEntry Entry;
                Entry.Script = StoreRef.Script;
                Entry.Variable = FNiagaraVariable(Variable.GetType(), Variable.GetName());
                if (ReadParameterBytes(Store, Entry.Variable, Entry.Data))
                {
                    Entries.Add(MoveTemp(Entry));
                }
            }
        }
    }

    int32 FRapidIterationValueSnapshot::MergeBack()
    {
        int32 Restored = 0;
        for (const FEntry& Entry : Entries)
        {
            UNiagaraScript* Script = Entry.Script.Get();
            if (!Script)
            {
                continue;
            }
            FNiagaraParameterStore& Store = Script->RapidIterationParameters;
            const FNiagaraVariableWithOffset* Current = Store.FindParameterVariable(Entry.Variable);
            if (!Current || Current->GetType() != Entry.Variable.GetType())
            {
                continue;
            }

            TArray<uint8> CurrentData;
            if (!ReadParameterBytes(Store, Entry.Variable, CurrentData)
                || CurrentData.Num() != Entry.Data.Num()
                || FMemory::Memcmp(CurrentData.GetData(), Entry.Data.GetData(), Entry.Data.Num()) == 0)
            {
                continue;
            }

            Script->Modify();
            Store.SetParameterData(Entry.Data.GetData(), Entry.Variable, /*bAdd=*/false);
            ++Restored;
        }
        return Restored;
    }
}
