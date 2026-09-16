// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/Niagara/NiagaraModelBuilder.h"

#include "Handlers/Asset/AssetDumpHandler.h"
#include "Handlers/Niagara/NiagaraJsonHelpers.h"
#include "NiagaraEmitter.h"
#include "NiagaraParameterStore.h"
#include "NiagaraSystem.h"
#include "Utils/JsonBuilders.h"
#include "Utils/PropertyUtils.h"
#include "Utils/PropertyInspection.h"

#include "NiagaraDataInterface.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraScript.h"
#include "UObject/UnrealType.h"

namespace
{
    using NiagaraJsonHelpers::MakeObject;
    using NiagaraJsonHelpers::MakeObjectValue;
    using NiagaraJsonHelpers::MakeStringValue;
    using NiagaraJsonHelpers::GetObjectPathSafe;
    using NiagaraJsonHelpers::GetClassPathSafe;
    using NiagaraJsonHelpers::BytesToHexString;
    using NiagaraJsonHelpers::BuildVector3fJson;
    using NiagaraJsonHelpers::BuildVector4fJson;
    using NiagaraJsonHelpers::BuildQuat4fJson;
    using JsonBuilders::BuildLinearColorJson;
    using NiagaraJsonHelpers::BuildTypeModel;

    constexpr int32 MaxInlineRawBytes = 256;

    // NOTE: 2-arg BuildOwnerRecord is unique to this file (display name derived
    // from Owner->GetName()). Other Niagara builders use the 3-arg overload in
    // NiagaraJsonHelpers; the two coexist via overload resolution.
    TSharedPtr<FJsonObject> BuildOwnerRecord(const FString& Kind, const UObject* Owner)
    {
        TSharedPtr<FJsonObject> Obj = MakeObject();
        Obj->SetStringField(TEXT("kind"), Kind);
        Obj->SetStringField(TEXT("displayName"), Owner ? Owner->GetName() : FString());
        Obj->SetStringField(TEXT("objectPath"), GetObjectPathSafe(Owner));
        Obj->SetStringField(TEXT("class"), GetClassPathSafe(Owner));
        return Obj;
    }

    TSharedPtr<FJsonObject> BuildParameterProvenance(const FString& Scope, const UObject* Owner)
    {
        TSharedPtr<FJsonObject> Obj = MakeObject();
        Obj->SetStringField(TEXT("rawParameterFile"), DumpFileNames::NiagaraParameters);
        TArray<TSharedPtr<FJsonValue>> RawDumpFiles;
        RawDumpFiles.Add(MakeStringValue(DumpFileNames::NiagaraParameters));
        Obj->SetArrayField(TEXT("rawDumpFiles"), RawDumpFiles);
        Obj->SetStringField(TEXT("scope"), Scope);
        Obj->SetObjectField(TEXT("owner"), BuildOwnerRecord(Scope, Owner));
        return Obj;
    }

    TSharedPtr<FJsonObject> BuildVector2fJson(const FVector2f& Value)
    {
        TSharedPtr<FJsonObject> Obj = MakeObject();
        Obj->SetNumberField(TEXT("x"), Value.X);
        Obj->SetNumberField(TEXT("y"), Value.Y);
        return Obj;
    }

    TSharedPtr<FJsonObject> BuildMatrix44fJson(const FMatrix44f& Value)
    {
        TSharedPtr<FJsonObject> Obj = MakeObject();
        for (int32 Row = 0; Row < 4; ++Row)
        {
            TArray<TSharedPtr<FJsonValue>> Values;
            for (int32 Column = 0; Column < 4; ++Column)
            {
                Values.Add(MakeShared<FJsonValueNumber>(Value.M[Row][Column]));
            }
            Obj->SetArrayField(FString::Printf(TEXT("row%d"), Row), Values);
        }
        return Obj;
    }

    TSharedPtr<FJsonObject> BuildObjectRefPayload(const UObject* Object)
    {
        TSharedPtr<FJsonObject> Obj = MakeObject();
        Obj->SetStringField(TEXT("_kind"), TEXT("UObject"));
        Obj->SetBoolField(TEXT("present"), Object != nullptr);
        Obj->SetStringField(TEXT("objectPath"), GetObjectPathSafe(Object));
        Obj->SetStringField(TEXT("class"), GetClassPathSafe(Object));
        return Obj;
    }

    TArray<TSharedPtr<FJsonValue>> BuildDataInterfaceUObjectRefs(const UNiagaraDataInterface* DataInterface)
    {
        TArray<TSharedPtr<FJsonValue>> Refs;
        if (!DataInterface)
        {
            return Refs;
        }

        TArray<FProperty*> Properties;
        for (TFieldIterator<FProperty> It(DataInterface->GetClass(), EFieldIteratorFlags::IncludeSuper); It; ++It)
        {
            FProperty* Property = *It;
            if (!Property || Property->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated))
            {
                continue;
            }
            if (CastField<FObjectProperty>(Property) || CastField<FClassProperty>(Property) || CastField<FSoftObjectProperty>(Property))
            {
                Properties.Add(Property);
            }
        }
        Properties.Sort([](const FProperty& A, const FProperty& B)
        {
            return A.GetName() < B.GetName();
        });

        for (FProperty* Property : Properties)
        {
            TSharedPtr<FJsonObject> Ref = MakeObject();
            Ref->SetStringField(TEXT("property"), Property->GetName());
            Ref->SetStringField(TEXT("type"), GetPropertyCppTypeWithParams(Property));
            if (TSharedPtr<FJsonValue> Value = ExportPropertyToJsonValue(const_cast<UNiagaraDataInterface*>(DataInterface), Property))
            {
                Ref->SetField(TEXT("value"), Value);
            }
            Refs.Add(MakeObjectValue(Ref));
        }
        return Refs;
    }

    TSharedPtr<FJsonObject> BuildDataInterfacePayload(const UNiagaraDataInterface* DataInterface, const FString& Scope, const UObject* Owner)
    {
        TSharedPtr<FJsonObject> Obj = MakeObject();
        Obj->SetStringField(TEXT("_kind"), TEXT("DataInterface"));
        Obj->SetBoolField(TEXT("present"), DataInterface != nullptr);
        Obj->SetStringField(TEXT("objectPath"), GetObjectPathSafe(DataInterface));
        Obj->SetStringField(TEXT("class"), GetClassPathSafe(DataInterface));
        Obj->SetObjectField(TEXT("owner"), BuildOwnerRecord(Scope, Owner));
        if (DataInterface)
        {
            Obj->SetObjectField(TEXT("properties"), BuildSparsePropertyDiffJson(
                const_cast<UNiagaraDataInterface*>(DataInterface),
                DataInterface->GetClass() ? DataInterface->GetClass()->GetDefaultObject() : nullptr));
            Obj->SetArrayField(TEXT("uObjectRefs"), BuildDataInterfaceUObjectRefs(DataInterface));
        }
        else
        {
            Obj->SetObjectField(TEXT("properties"), MakeObject());
            Obj->SetArrayField(TEXT("uObjectRefs"), TArray<TSharedPtr<FJsonValue>>());
        }
        Obj->SetObjectField(TEXT("provenance"), BuildParameterProvenance(Scope, Owner));
        return Obj;
    }

    TSharedPtr<FJsonObject> BuildRawBytesPayload(const uint8* Data, int32 Size)
    {
        TSharedPtr<FJsonObject> Obj = MakeObject();
        Obj->SetStringField(TEXT("_kind"), TEXT("rawBytes"));
        Obj->SetNumberField(TEXT("sizeBytes"), Size);
        Obj->SetNumberField(TEXT("inlineBytes"), FMath::Clamp(Size, 0, MaxInlineRawBytes));
        Obj->SetBoolField(TEXT("truncated"), Size > MaxInlineRawBytes);
        Obj->SetStringField(TEXT("hex"), Data && Size > 0 ? BytesToHexString(Data, FMath::Min(Size, MaxInlineRawBytes)) : FString());
        return Obj;
    }

    TSharedPtr<FJsonValue> BuildTypedValue(const FNiagaraParameterStore& Store, const FNiagaraVariableWithOffset& Variable, const FString& Scope, const UObject* Owner)
    {
        const FNiagaraTypeDefinition& Type = Variable.GetType();
        if (Type.IsDataInterface())
        {
            const TArray<UNiagaraDataInterface*>& DataInterfaces = Store.GetDataInterfaces();
            const UNiagaraDataInterface* DataInterface = DataInterfaces.IsValidIndex(Variable.Offset) ? DataInterfaces[Variable.Offset] : nullptr;
            return MakeObjectValue(BuildDataInterfacePayload(DataInterface, Scope, Owner));
        }

        if (Type.IsUObject())
        {
            // GetUObjects() returns TArray<UObject*> in UE 5.4 and TArray<TObjectPtr<UObject>> in UE 5.6+.
            // auto avoids a hard-typed declaration incompatible with 5.4. Both element types
            // implicitly convert to UObject* so the explicit cast below compiles on all versions.
            const auto& Objects = Store.GetUObjects();
            const UObject* Object = Objects.IsValidIndex(Variable.Offset) ? static_cast<const UObject*>(Objects[Variable.Offset]) : nullptr;
            return MakeObjectValue(BuildObjectRefPayload(Object));
        }

        const uint8* Data = Store.GetParameterData(Variable);
        const int32 Size = Variable.GetSizeInBytes();
        if (!Data || Size <= 0)
        {
            return MakeShared<FJsonValueNull>();
        }

        if (Type == FNiagaraTypeDefinition::GetFloatDef() && Size == sizeof(float))
        {
            float Value = 0.0f;
            FMemory::Memcpy(&Value, Data, sizeof(float));
            return MakeShared<FJsonValueNumber>(Value);
        }
        if (Type == FNiagaraTypeDefinition::GetIntDef() && Size == sizeof(int32))
        {
            int32 Value = 0;
            FMemory::Memcpy(&Value, Data, sizeof(int32));
            return MakeShared<FJsonValueNumber>(Value);
        }
        if (Type.IsSameBaseDefinition(FNiagaraTypeDefinition::GetBoolDef()) && Size >= 1)
        {
            bool Value = false;
            for (int32 Index = 0; Index < Size; ++Index)
            {
                if (Data[Index] != 0)
                {
                    Value = true;
                    break;
                }
            }
            return MakeShared<FJsonValueBoolean>(Value);
        }
        if (Type == FNiagaraTypeDefinition::GetVec2Def() && Size == sizeof(FVector2f))
        {
            FVector2f Value;
            FMemory::Memcpy(&Value, Data, sizeof(FVector2f));
            return MakeObjectValue(BuildVector2fJson(Value));
        }
        if ((Type == FNiagaraTypeDefinition::GetVec3Def() || Type == FNiagaraTypeDefinition::GetPositionDef()) && Size == sizeof(FVector3f))
        {
            FVector3f Value;
            FMemory::Memcpy(&Value, Data, sizeof(FVector3f));
            return MakeObjectValue(BuildVector3fJson(Value));
        }
        if (Type == FNiagaraTypeDefinition::GetVec4Def() && Size == sizeof(FVector4f))
        {
            FVector4f Value;
            FMemory::Memcpy(&Value, Data, sizeof(FVector4f));
            return MakeObjectValue(BuildVector4fJson(Value));
        }
        if (Type == FNiagaraTypeDefinition::GetQuatDef() && Size == sizeof(FQuat4f))
        {
            FQuat4f Value;
            FMemory::Memcpy(&Value, Data, sizeof(FQuat4f));
            return MakeObjectValue(BuildQuat4fJson(Value));
        }
        if (Type == FNiagaraTypeDefinition::GetColorDef() && Size == sizeof(FLinearColor))
        {
            FLinearColor Value;
            FMemory::Memcpy(&Value, Data, sizeof(FLinearColor));
            return MakeObjectValue(BuildLinearColorJson(Value));
        }
        if (Type == FNiagaraTypeDefinition::GetMatrix4Def() && Size == sizeof(FMatrix44f))
        {
            FMatrix44f Value;
            FMemory::Memcpy(&Value, Data, sizeof(FMatrix44f));
            return MakeObjectValue(BuildMatrix44fJson(Value));
        }

        return MakeObjectValue(BuildRawBytesPayload(Data, Size));
    }

    TSharedPtr<FJsonObject> BuildParameterRecord(const FNiagaraParameterStore& Store, const FNiagaraVariableWithOffset& Variable, const FString& Scope, const UObject* Owner)
    {
        TSharedPtr<FJsonObject> Obj = MakeObject();
        Obj->SetStringField(TEXT("scope"), Scope);
        Obj->SetStringField(TEXT("name"), Variable.GetName().ToString());
        Obj->SetObjectField(TEXT("owner"), BuildOwnerRecord(Scope, Owner));
        Obj->SetObjectField(TEXT("type"), BuildTypeModel(Variable.GetType()));
        Obj->SetNumberField(TEXT("offset"), Variable.Offset);
        Obj->SetNumberField(TEXT("sizeBytes"), Variable.GetSizeInBytes());
        Obj->SetStringField(TEXT("storage"), Variable.GetType().IsDataInterface() ? TEXT("dataInterface") : Variable.GetType().IsUObject() ? TEXT("uObject") : TEXT("parameterData"));
        Obj->SetField(TEXT("value"), BuildTypedValue(Store, Variable, Scope, Owner));
        Obj->SetObjectField(TEXT("provenance"), BuildParameterProvenance(Scope, Owner));
        return Obj;
    }

    void AddDiagnostic(TArray<TSharedPtr<FJsonValue>>& Diagnostics, const FString& Code, const FString& Scope)
    {
        TSharedPtr<FJsonObject> Obj = MakeObject();
        Obj->SetStringField(TEXT("severity"), TEXT("warning"));
        Obj->SetStringField(TEXT("code"), Code);
        Obj->SetStringField(TEXT("scope"), Scope);
        Diagnostics.Add(MakeObjectValue(Obj));
    }

    TArray<TSharedPtr<FJsonValue>> BuildRapidIterationStore(const UNiagaraScript* Script, const FString& Scope, const UObject* Owner, TArray<TSharedPtr<FJsonValue>>& Diagnostics)
    {
        if (!Script)
        {
            AddDiagnostic(Diagnostics, TEXT("RAPID_ITERATION_STORE_NOT_ACCESSIBLE"), Scope);
            return TArray<TSharedPtr<FJsonValue>>();
        }
        return NiagaraModelBuilder::BuildParameterStoreModel(Script->RapidIterationParameters, Scope, Owner);
    }

    TSharedPtr<FJsonObject> BuildEmitterParameterModelFromData(const FVersionedNiagaraEmitterData* EmitterData, const UObject* Owner, const FString& OwnerScope)
    {
        TSharedPtr<FJsonObject> Obj = MakeObject();
        Obj->SetStringField(TEXT("scope"), OwnerScope);
        Obj->SetObjectField(TEXT("owner"), BuildOwnerRecord(OwnerScope, Owner));

        TArray<TSharedPtr<FJsonValue>> Diagnostics;
        Obj->SetArrayField(TEXT("rendererBindings"), EmitterData ? NiagaraModelBuilder::BuildParameterStoreModel(EmitterData->RendererBindings, TEXT("rendererBindings"), Owner) : TArray<TSharedPtr<FJsonValue>>());
        if (!EmitterData)
        {
            AddDiagnostic(Diagnostics, TEXT("EMITTER_DATA_NOT_ACCESSIBLE"), OwnerScope);
        }

        TSharedPtr<FJsonObject> RapidIteration = MakeObject();
        RapidIteration->SetArrayField(TEXT("emitterSpawn"), BuildRapidIterationStore(EmitterData ? EmitterData->EmitterSpawnScriptProps.Script : nullptr, TEXT("emitterSpawnRapidIteration"), Owner, Diagnostics));
        RapidIteration->SetArrayField(TEXT("emitterUpdate"), BuildRapidIterationStore(EmitterData ? EmitterData->EmitterUpdateScriptProps.Script : nullptr, TEXT("emitterUpdateRapidIteration"), Owner, Diagnostics));
        RapidIteration->SetArrayField(TEXT("particleSpawn"), BuildRapidIterationStore(EmitterData ? EmitterData->SpawnScriptProps.Script : nullptr, TEXT("particleSpawnRapidIteration"), Owner, Diagnostics));
        RapidIteration->SetArrayField(TEXT("particleUpdate"), BuildRapidIterationStore(EmitterData ? EmitterData->UpdateScriptProps.Script : nullptr, TEXT("particleUpdateRapidIteration"), Owner, Diagnostics));
        Obj->SetObjectField(TEXT("rapidIteration"), RapidIteration);
        Obj->SetArrayField(TEXT("diagnostics"), Diagnostics);
        Obj->SetObjectField(TEXT("provenance"), BuildParameterProvenance(OwnerScope, Owner));
        return Obj;
    }
}

namespace NiagaraModelBuilder
{
    TArray<TSharedPtr<FJsonValue>> BuildParameterStoreModel(const FNiagaraParameterStore& Store, const FString& Scope, const UObject* Owner)
    {
        TArray<TSharedPtr<FJsonValue>> Result;
        for (const FNiagaraVariableWithOffset& Variable : Store.ReadParameterVariables())
        {
            Result.Add(MakeObjectValue(BuildParameterRecord(Store, Variable, Scope, Owner)));
        }
        Result.Sort([](const TSharedPtr<FJsonValue>& A, const TSharedPtr<FJsonValue>& B)
        {
            const TSharedPtr<FJsonObject> AObj = A->AsObject();
            const TSharedPtr<FJsonObject> BObj = B->AsObject();
            FString AName;
            FString BName;
            AObj->TryGetStringField(TEXT("name"), AName);
            BObj->TryGetStringField(TEXT("name"), BName);
            return AName < BName;
        });
        return Result;
    }

    TSharedPtr<FJsonObject> BuildSystemParameterModel(const UNiagaraSystem* System)
    {
        TSharedPtr<FJsonObject> Obj = MakeObject();
        Obj->SetStringField(TEXT("scope"), TEXT("system"));
        Obj->SetObjectField(TEXT("owner"), BuildOwnerRecord(TEXT("system"), System));
        Obj->SetObjectField(TEXT("provenance"), BuildParameterProvenance(TEXT("system"), System));
        TArray<TSharedPtr<FJsonValue>> Diagnostics;
        Obj->SetArrayField(TEXT("user"), System ? BuildParameterStoreModel(System->GetExposedParameters(), TEXT("user"), System) : TArray<TSharedPtr<FJsonValue>>());
        Obj->SetArrayField(TEXT("systemSpawnRapidIteration"), BuildRapidIterationStore(System ? System->GetSystemSpawnScript() : nullptr, TEXT("systemSpawnRapidIteration"), System, Diagnostics));
        Obj->SetArrayField(TEXT("systemUpdateRapidIteration"), BuildRapidIterationStore(System ? System->GetSystemUpdateScript() : nullptr, TEXT("systemUpdateRapidIteration"), System, Diagnostics));

        TArray<TSharedPtr<FJsonValue>> Emitters;
        if (System)
        {
            for (const FNiagaraEmitterHandle& Handle : System->GetEmitterHandles())
            {
                const FVersionedNiagaraEmitter Instance = Handle.GetInstance();
                TSharedPtr<FJsonObject> EmitterObj = BuildEmitterParameterModelFromData(Handle.GetEmitterData(), Instance.Emitter, TEXT("emitter"));
                EmitterObj->SetStringField(TEXT("name"), Handle.GetName().ToString());
                EmitterObj->SetStringField(TEXT("id"), Handle.GetId().ToString());
                EmitterObj->SetStringField(TEXT("idName"), Handle.GetIdName().ToString());
                Emitters.Add(MakeObjectValue(EmitterObj));
            }
        }
        else
        {
            AddDiagnostic(Diagnostics, TEXT("SYSTEM_NOT_ACCESSIBLE"), TEXT("system"));
        }
        Obj->SetArrayField(TEXT("emitters"), Emitters);
        Obj->SetArrayField(TEXT("diagnostics"), Diagnostics);
        return Obj;
    }

    TSharedPtr<FJsonObject> BuildEmitterParameterModel(const UNiagaraEmitter* Emitter)
    {
        return BuildEmitterParameterModelFromData(Emitter ? Emitter->GetLatestEmitterData() : nullptr, Emitter, TEXT("emitterAsset"));
    }
}
