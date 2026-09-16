// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/Niagara/NiagaraDumpBuilder.h"

#include "Handlers/Niagara/NiagaraDecompileHelpers.h"
#include "Handlers/Niagara/NiagaraEditTypes.h"
#include "Handlers/Niagara/NiagaraGraphResetUtils.h"
#include "Handlers/Niagara/NiagaraJsonHelpers.h"
#include "Handlers/Niagara/NiagaraRapidIteration.h"
#include "HAL/IConsoleManager.h"
#include "Math/Float16.h"
#include "Compat/EngineVersionCompat.h"
#include "Utils/JsonUtils.h"
#include "Utils/PropertyUtils.h"

#include "NiagaraEffectType.h"
#include "NiagaraEmitter.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraDataInterface.h"
#include "NiagaraGraph.h"
#include "NiagaraNode.h"
#include "NiagaraNodeFunctionCall.h"
#include "NiagaraNodeInput.h"
#include "NiagaraNodeOutput.h"
#include "NiagaraNodeStaticSwitch.h"
#include "NiagaraParameterStore.h"
#include "NiagaraPlatformSet.h"
#include "NiagaraRendererProperties.h"
#include "NiagaraScript.h"
#include "NiagaraScriptSource.h"
#include "NiagaraScriptVariable.h"
#include "NiagaraSimulationStageBase.h"
#include "NiagaraSystem.h"
#include "ViewModels/Stack/NiagaraParameterHandle.h"
#include "UObject/StructOnScope.h"
#include "UObject/UnrealType.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"

namespace
{
    using namespace NiagaraJsonHelpers;

    TSharedPtr<FJsonObject> BuildVectorJson(const FVector& Value)
    {
        TSharedPtr<FJsonObject> Obj = MakeObject();
        Obj->SetNumberField(TEXT("x"), Value.X);
        Obj->SetNumberField(TEXT("y"), Value.Y);
        Obj->SetNumberField(TEXT("z"), Value.Z);
        return Obj;
    }

    TSharedPtr<FJsonObject> BuildBoxJson(const FBox& Box)
    {
        TSharedPtr<FJsonObject> Obj = MakeObject();
        Obj->SetBoolField(TEXT("isValid"), Box.IsValid != 0);
        Obj->SetObjectField(TEXT("min"), BuildVectorJson(Box.Min));
        Obj->SetObjectField(TEXT("max"), BuildVectorJson(Box.Max));
        return Obj;
    }

    TSharedPtr<FJsonObject> BuildAssetVersionJson(const FNiagaraAssetVersion& Version)
    {
        TSharedPtr<FJsonObject> Obj = MakeObject();
        Obj->SetNumberField(TEXT("major"), Version.MajorVersion);
        Obj->SetNumberField(TEXT("minor"), Version.MinorVersion);
        Obj->SetStringField(TEXT("guid"), Version.VersionGuid.ToString());
        Obj->SetBoolField(TEXT("visibleInVersionSelector"), Version.bIsVisibleInVersionSelector);
        return Obj;
    }

    TSharedPtr<FJsonObject> BuildCVarConditionJson(const FNiagaraPlatformSetCVarCondition& Condition)
    {
        TSharedPtr<FJsonObject> Obj = MakeObject();
        Obj->SetStringField(TEXT("cvar"), Condition.CVarName.ToString());
        Obj->SetStringField(TEXT("passResponse"), EnumToString(Condition.PassResponse));
        Obj->SetStringField(TEXT("failResponse"), EnumToString(Condition.FailResponse));
        Obj->SetBoolField(TEXT("requiredBoolValue"), Condition.Value);
        Obj->SetBoolField(TEXT("useMinInt"), Condition.bUseMinInt != 0);
        Obj->SetBoolField(TEXT("useMaxInt"), Condition.bUseMaxInt != 0);
        Obj->SetBoolField(TEXT("useMinFloat"), Condition.bUseMinFloat != 0);
        Obj->SetBoolField(TEXT("useMaxFloat"), Condition.bUseMaxFloat != 0);
        Obj->SetNumberField(TEXT("minInt"), Condition.MinInt);
        Obj->SetNumberField(TEXT("maxInt"), Condition.MaxInt);
        Obj->SetNumberField(TEXT("minFloat"), Condition.MinFloat);
        Obj->SetNumberField(TEXT("maxFloat"), Condition.MaxFloat);
        return Obj;
    }

    TSharedPtr<FJsonObject> BuildDeviceProfileStateJson(const FNiagaraDeviceProfileStateEntry& Entry)
    {
        TSharedPtr<FJsonObject> Obj = MakeObject();
        Obj->SetStringField(TEXT("profileName"), Entry.ProfileName.ToString());
        Obj->SetNumberField(TEXT("qualityLevelMask"), Entry.QualityLevelMask);
        Obj->SetNumberField(TEXT("setQualityLevelMask"), Entry.SetQualityLevelMask);
        return Obj;
    }

    TSharedPtr<FJsonObject> BuildPlatformSetJson(const FNiagaraPlatformSet& Set)
    {
        TSharedPtr<FJsonObject> Obj = MakeObject();
        Obj->SetNumberField(TEXT("qualityLevelMask"), Set.QualityLevelMask);

        // Per-override platform-set bloat: omit empty arrays since most overrides leave them empty.
        if (Set.DeviceProfileStates.Num() > 0)
        {
            TArray<TSharedPtr<FJsonValue>> Profiles;
            Profiles.Reserve(Set.DeviceProfileStates.Num());
            for (const FNiagaraDeviceProfileStateEntry& Entry : Set.DeviceProfileStates)
            {
                Profiles.Add(MakeObjectValue(BuildDeviceProfileStateJson(Entry)));
            }
            Obj->SetArrayField(TEXT("deviceProfileStates"), Profiles);
        }

        if (Set.CVarConditions.Num() > 0)
        {
            TArray<TSharedPtr<FJsonValue>> CVars;
            CVars.Reserve(Set.CVarConditions.Num());
            for (const FNiagaraPlatformSetCVarCondition& Condition : Set.CVarConditions)
            {
                CVars.Add(MakeObjectValue(BuildCVarConditionJson(Condition)));
            }
            Obj->SetArrayField(TEXT("cvarConditions"), CVars);
        }
        return Obj;
    }

    TSharedPtr<FJsonObject> BuildVisibilityCullingJson(const FNiagaraSystemVisibilityCullingSettings& Settings)
    {
        TSharedPtr<FJsonObject> Obj = MakeObject();
        Obj->SetBoolField(TEXT("bCullWhenNotRendered"), Settings.bCullWhenNotRendered != 0);
        Obj->SetBoolField(TEXT("bCullByViewFrustum"), Settings.bCullByViewFrustum != 0);
        Obj->SetBoolField(TEXT("bAllowPreCullingByViewFrustum"), Settings.bAllowPreCullingByViewFrustum != 0);
        Obj->SetNumberField(TEXT("maxTimeOutsideViewFrustum"), Settings.MaxTimeOutsideViewFrustum);
        Obj->SetNumberField(TEXT("maxTimeWithoutRender"), Settings.MaxTimeWithoutRender);
        return Obj;
    }

    TSharedPtr<FJsonObject> BuildLinearRampJson(const FNiagaraLinearRamp& Ramp)
    {
        TSharedPtr<FJsonObject> Obj = MakeObject();
        Obj->SetNumberField(TEXT("startX"), Ramp.StartX);
        Obj->SetNumberField(TEXT("startY"), Ramp.StartY);
        Obj->SetNumberField(TEXT("endX"), Ramp.EndX);
        Obj->SetNumberField(TEXT("endY"), Ramp.EndY);
        return Obj;
    }

    TSharedPtr<FJsonObject> BuildBudgetScalingJson(const FNiagaraGlobalBudgetScaling& Scaling)
    {
        TSharedPtr<FJsonObject> Obj = MakeObject();
        Obj->SetBoolField(TEXT("bCullByGlobalBudget"), Scaling.bCullByGlobalBudget != 0);
        Obj->SetBoolField(TEXT("bScaleMaxDistanceByGlobalBudgetUse"), Scaling.bScaleMaxDistanceByGlobalBudgetUse != 0);
        Obj->SetBoolField(TEXT("bScaleMaxInstanceCountByGlobalBudgetUse"), Scaling.bScaleMaxInstanceCountByGlobalBudgetUse != 0);
        Obj->SetBoolField(TEXT("bScaleSystemInstanceCountByGlobalBudgetUse"), Scaling.bScaleSystemInstanceCountByGlobalBudgetUse != 0);
        Obj->SetNumberField(TEXT("maxGlobalBudgetUsage"), Scaling.MaxGlobalBudgetUsage);
        Obj->SetObjectField(TEXT("maxDistanceScaleByGlobalBudgetUse"), BuildLinearRampJson(Scaling.MaxDistanceScaleByGlobalBudgetUse));
        Obj->SetObjectField(TEXT("maxInstanceCountScaleByGlobalBudgetUse"), BuildLinearRampJson(Scaling.MaxInstanceCountScaleByGlobalBudgetUse));
        Obj->SetObjectField(TEXT("maxSystemInstanceCountScaleByGlobalBudgetUse"), BuildLinearRampJson(Scaling.MaxSystemInstanceCountScaleByGlobalBudgetUse));
        return Obj;
    }

    TSharedPtr<FJsonObject> BuildSystemScalabilityOverrideJson(const FNiagaraSystemScalabilityOverride& Override)
    {
        TSharedPtr<FJsonObject> Obj = MakeObject();
        Obj->SetBoolField(TEXT("bOverrideDistanceSettings"), Override.bOverrideDistanceSettings != 0);
        Obj->SetBoolField(TEXT("bOverrideInstanceCountSettings"), Override.bOverrideInstanceCountSettings != 0);
        Obj->SetBoolField(TEXT("bOverridePerSystemInstanceCountSettings"), Override.bOverridePerSystemInstanceCountSettings != 0);
        Obj->SetBoolField(TEXT("bOverrideVisibilitySettings"), Override.bOverrideVisibilitySettings != 0);
        Obj->SetBoolField(TEXT("bOverrideGlobalBudgetScalingSettings"), Override.bOverrideGlobalBudgetScalingSettings != 0);
        Obj->SetBoolField(TEXT("bOverrideCullProxySettings"), Override.bOverrideCullProxySettings != 0);

        Obj->SetBoolField(TEXT("bCullByDistance"), Override.bCullByDistance != 0);
        Obj->SetBoolField(TEXT("bCullMaxInstanceCount"), Override.bCullMaxInstanceCount != 0);
        Obj->SetBoolField(TEXT("bCullPerSystemMaxInstanceCount"), Override.bCullPerSystemMaxInstanceCount != 0);
        Obj->SetNumberField(TEXT("maxDistance"), Override.MaxDistance);
        Obj->SetNumberField(TEXT("maxInstances"), Override.MaxInstances);
        Obj->SetNumberField(TEXT("maxSystemInstances"), Override.MaxSystemInstances);
        Obj->SetStringField(TEXT("cullProxyMode"), EnumToString(Override.CullProxyMode));
        Obj->SetNumberField(TEXT("maxSystemProxies"), Override.MaxSystemProxies);

        Obj->SetObjectField(TEXT("visibility"), BuildVisibilityCullingJson(Override.VisibilityCulling));
        Obj->SetObjectField(TEXT("budgetScaling"), BuildBudgetScalingJson(Override.BudgetScaling));
        Obj->SetObjectField(TEXT("platforms"), BuildPlatformSetJson(Override.Platforms));
        return Obj;
    }

    TSharedPtr<FJsonObject> BuildEmitterScalabilityOverrideJson(const FNiagaraEmitterScalabilityOverride& Override)
    {
        TSharedPtr<FJsonObject> Obj = MakeObject();
        Obj->SetBoolField(TEXT("bOverrideSpawnCountScale"), Override.bOverrideSpawnCountScale != 0);
        Obj->SetBoolField(TEXT("bScaleSpawnCount"), Override.bScaleSpawnCount != 0);
        Obj->SetNumberField(TEXT("spawnCountScale"), Override.SpawnCountScale);
        Obj->SetObjectField(TEXT("platforms"), BuildPlatformSetJson(Override.Platforms));
        return Obj;
    }

    TSharedPtr<FJsonObject> BuildScriptJson(const UNiagaraScript* Script)
    {
        TSharedPtr<FJsonObject> Obj = MakeObject();
        if (!Script)
        {
            Obj->SetBoolField(TEXT("present"), false);
            return Obj;
        }

        Obj->SetBoolField(TEXT("present"), true);
        Obj->SetStringField(TEXT("name"), Script->GetName());
        Obj->SetStringField(TEXT("path"), Script->GetPathName());
        Obj->SetStringField(TEXT("usage"), EnumToString(Script->GetUsage()));
        Obj->SetStringField(TEXT("usageId"), Script->GetUsageId().ToString());
        // NCS_Unknown is the engine's pre-compile sentinel; surface it as JSON null so consumers
        // do not mistake "we have not compiled yet" for "compile produced status NCS_Unknown".
        const ENiagaraScriptCompileStatus CompileStatus = Script->GetLastCompileStatus();
        if (CompileStatus == ENiagaraScriptCompileStatus::NCS_Unknown)
        {
            Obj->SetField(TEXT("compileStatus"), MakeShared<FJsonValueNull>());
        }
        else
        {
            Obj->SetStringField(TEXT("compileStatus"), EnumToString(CompileStatus));
        }

        // A script the compiler rejected keeps its diagnostics in the cached executable data.
        // Published beside the status so a caller that reads NCS_Error learns WHY here rather
        // than by grepping the editor log for "errors encountered compiling Vector VM shaders",
        // which was the only place the generated-HLSL errors ever appeared
        // (B-niagara-validate-green-while-scripts-ncs-error). Emitted even when the list comes
        // back empty: an NCS_Error whose messages did not survive is still a failed compile, and
        // an absent field would be indistinguishable from a script that never failed.
        if (CompileStatus == ENiagaraScriptCompileStatus::NCS_Error)
        {
            TArray<TSharedPtr<FJsonValue>> CompileErrors;
#if WITH_EDITORONLY_DATA
            const FNiagaraVMExecutableData& ExecutableData = Script->GetVMExecutableData();
            for (const FNiagaraCompileEvent& Event : ExecutableData.LastCompileEvents)
            {
                if (Event.Severity == FNiagaraCompileEventSeverity::Error)
                {
                    CompileErrors.Add(MakeStringValue(Event.Message));
                }
            }
            // ErrorMsg is the single-string fallback the translator fills when it fails before
            // it can emit per-node events; only useful when there are no events to report.
            if (CompileErrors.Num() == 0 && !ExecutableData.ErrorMsg.IsEmpty())
            {
                CompileErrors.Add(MakeStringValue(ExecutableData.ErrorMsg));
            }
#endif
            Obj->SetArrayField(TEXT("compileErrors"), CompileErrors);
        }

        if (const UNiagaraScriptSource* Source = Cast<UNiagaraScriptSource>(Script->GetLatestSource()))
        {
            Obj->SetStringField(TEXT("sourcePath"), Source->GetPathName());
            if (Source->NodeGraph)
            {
                Obj->SetStringField(TEXT("graphPath"), Source->NodeGraph->GetPathName());
                Obj->SetStringField(TEXT("graphName"), Source->NodeGraph->GetName());
            }
        }

        return Obj;
    }

    bool IsSystemReadyToRunWithoutLoading(const UNiagaraSystem* System)
    {
        if (!System)
        {
            return false;
        }

        const UNiagaraScript* SystemSpawnScript = System->GetSystemSpawnScript();
        const UNiagaraScript* SystemUpdateScript = System->GetSystemUpdateScript();
        if (!SystemSpawnScript || !SystemUpdateScript || System->HasOutstandingCompilationRequests())
        {
            return false;
        }

        for (const FNiagaraEmitterHandle& Handle : System->GetEmitterHandles())
        {
            const FVersionedNiagaraEmitterData* EmitterData = Handle.GetEmitterData();
            if (EmitterData && !EmitterData->IsReadyToRun())
            {
                return false;
            }
        }

        return true;
    }

    TSharedPtr<FJsonObject> BuildVariableTypeJson(const FNiagaraTypeDefinition& Type)
    {
        TSharedPtr<FJsonObject> Obj = MakeObject();
        Obj->SetStringField(TEXT("name"), Type.GetName());
        Obj->SetNumberField(TEXT("sizeBytes"), Type.GetSize());
        Obj->SetBoolField(TEXT("isDataInterface"), Type.IsDataInterface());
        Obj->SetBoolField(TEXT("isUObject"), Type.IsUObject());
        if (UStruct* Struct = Type.GetStruct())
        {
            Obj->SetStringField(TEXT("struct"), Struct->GetPathName());
        }
        if (UClass* Class = Type.GetClass())
        {
            Obj->SetStringField(TEXT("class"), Class->GetPathName());
        }
        return Obj;
    }

    TSharedPtr<FJsonValue> BuildParameterValueJson(const FNiagaraParameterStore& Store, const FNiagaraVariableWithOffset& Variable)
    {
        const FNiagaraTypeDefinition& Type = Variable.GetType();
        if (Type.IsDataInterface())
        {
            const TArray<UNiagaraDataInterface*>& DataInterfaces = Store.GetDataInterfaces();
            if (DataInterfaces.IsValidIndex(Variable.Offset))
            {
                if (UNiagaraDataInterface* DataInterface = DataInterfaces[Variable.Offset])
                {
                    TSharedPtr<FJsonObject> Obj = MakeObject();
                    Obj->SetStringField(TEXT("path"), DataInterface->GetPathName());
                    Obj->SetStringField(TEXT("class"), DataInterface->GetClass()->GetPathName());
                    Obj->SetObjectField(TEXT("properties"), BuildSparsePropertyDiffJson(DataInterface, DataInterface->GetClass()->GetDefaultObject()));
                    return MakeObjectValue(Obj);
                }
            }
            return MakeShared<FJsonValueNull>();
        }

        if (Type.IsUObject())
        {
            // GetUObjects() returns TArray<UObject*> in UE 5.4 and TArray<TObjectPtr<UObject>> in UE 5.6+.
            // Using auto avoids a hard-typed declaration that fails on 5.4; both element types
            // implicitly convert to UObject* so the inner cast compiles on all versions.
            const auto& Objects = Store.GetUObjects();
            if (Objects.IsValidIndex(Variable.Offset))
            {
                if (UObject* Object = Objects[Variable.Offset])
                {
                    return MakeShared<FJsonValueString>(Object->GetPathName());
                }
                return MakeShared<FJsonValueNull>();
            }
            return MakeShared<FJsonValueNull>();
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
        if (Type.IsSameBaseDefinition(FNiagaraTypeDefinition::GetBoolDef()) && Size >= 1)
        {
            // byte-OR handles both FNiagaraBool 4-byte INDEX_NONE/0 form and 1-byte parameter-store representation.
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
            return JsonVec2(Value);
        }
        if (Type == FNiagaraTypeDefinition::GetIDDef() && Size == sizeof(FNiagaraID))
        {
            FNiagaraID Value;
            FMemory::Memcpy(&Value, Data, sizeof(FNiagaraID));
            TSharedPtr<FJsonObject> Obj = MakeObject();
            Obj->SetNumberField(TEXT("index"), Value.Index);
            Obj->SetNumberField(TEXT("acquireTag"), Value.AcquireTag);
            return MakeObjectValue(Obj);
        }
        if (Type.GetScriptStruct() == FNiagaraSpawnInfo::StaticStruct() && Size == sizeof(FNiagaraSpawnInfo))
        {
            FNiagaraSpawnInfo Value;
            FMemory::Memcpy(&Value, Data, sizeof(FNiagaraSpawnInfo));
            TSharedPtr<FJsonObject> Obj = MakeObject();
            Obj->SetNumberField(TEXT("count"), Value.Count);
            Obj->SetNumberField(TEXT("interpStartDt"), Value.InterpStartDt);
            Obj->SetNumberField(TEXT("intervalDt"), Value.IntervalDt);
            Obj->SetNumberField(TEXT("spawnGroup"), Value.SpawnGroup);
            return MakeObjectValue(Obj);
        }
        if (Type == FNiagaraTypeDefinition::GetHalfDef() && Size == sizeof(uint16))
        {
            FFloat16 Value;
            FMemory::Memcpy(&Value.Encoded, Data, sizeof(uint16));
            return JsonHalf(Value);
        }
        if (Type == FNiagaraTypeDefinition::GetHalfVec2Def() && Size == sizeof(uint16) * 2)
        {
            // FNiagaraHalfVector2 layout matches two consecutive uint16 (FFloat16::Encoded).
            FFloat16 X;
            FFloat16 Y;
            FMemory::Memcpy(&X.Encoded, Data, sizeof(uint16));
            FMemory::Memcpy(&Y.Encoded, Data + sizeof(uint16), sizeof(uint16));
            return JsonHalfVec2(X, Y);
        }
        if (Type == FNiagaraTypeDefinition::GetHalfVec3Def() && Size == sizeof(uint16) * 3)
        {
            FFloat16 X;
            FFloat16 Y;
            FFloat16 Z;
            FMemory::Memcpy(&X.Encoded, Data + 0 * sizeof(uint16), sizeof(uint16));
            FMemory::Memcpy(&Y.Encoded, Data + 1 * sizeof(uint16), sizeof(uint16));
            FMemory::Memcpy(&Z.Encoded, Data + 2 * sizeof(uint16), sizeof(uint16));
            return JsonHalfVec3(X, Y, Z);
        }
        if (Type == FNiagaraTypeDefinition::GetHalfVec4Def() && Size == sizeof(uint16) * 4)
        {
            FFloat16 X;
            FFloat16 Y;
            FFloat16 Z;
            FFloat16 W;
            FMemory::Memcpy(&X.Encoded, Data + 0 * sizeof(uint16), sizeof(uint16));
            FMemory::Memcpy(&Y.Encoded, Data + 1 * sizeof(uint16), sizeof(uint16));
            FMemory::Memcpy(&Z.Encoded, Data + 2 * sizeof(uint16), sizeof(uint16));
            FMemory::Memcpy(&W.Encoded, Data + 3 * sizeof(uint16), sizeof(uint16));
            return JsonHalfVec4(X, Y, Z, W);
        }

        if (UScriptStruct* Struct = Type.GetScriptStruct())
        {
            FStructOnScope StructScope(Struct);
            uint8* StructData = StructScope.GetStructMemory();

            const FNiagaraVariable SourceVariable(Type, Variable.GetName());
            if (StructData && Store.CopyParameterData(SourceVariable, StructData))
            {
                TSharedPtr<FJsonObject> Fields = MakeObject();
                for (TFieldIterator<FProperty> It(Struct, EFieldIteratorFlags::IncludeSuper); It; ++It)
                {
                    FProperty* Property = *It;
                    if (TSharedPtr<FJsonValue> FieldValue = ExportPropertyToJsonValue(StructData, Property))
                    {
                        Fields->SetField(Property->GetName(), FieldValue);
                    }
                }

                TSharedPtr<FJsonObject> Obj = MakeObject();
                Obj->SetStringField(TEXT("_kind"), TEXT("struct"));
                Obj->SetStringField(TEXT("scriptStruct"), Struct->GetPathName());
                Obj->SetObjectField(TEXT("fields"), Fields);
                return MakeObjectValue(Obj);
            }
        }

        TSharedPtr<FJsonObject> Raw = MakeObject();
        Raw->SetStringField(TEXT("_kind"), TEXT("rawBytes"));
        Raw->SetNumberField(TEXT("sizeBytes"), Size);
        Raw->SetStringField(TEXT("hex"), BytesToHexString(Data, Size));
        return MakeObjectValue(Raw);
    }

    // One module-input override pin standing over the rapid-iteration constant of the same name,
    // keyed by that constant so a store entry can be marked as not being the only source for its
    // input. Built by BuildRapidIterationOverrideIndex below.
    struct FRapidIterationOverrideMark
    {
        // The stack aspect's moduleInputs[].valueMode for the same input.
        FString ValueMode;
        // Pin-default text; set only for valueMode "local".
        FString Value;
        // Emitter name, or empty for a module in a system stage.
        FString Emitter;
        FString Module;
        FString Input;
    };

    using FRapidIterationOverrideIndex = TMap<FName, FRapidIterationOverrideMark>;

    TArray<TSharedPtr<FJsonValue>> BuildParameterStoreArray(
        const FNiagaraParameterStore& Store,
        const FString& Scope,
        const FString& NameFilter,
        const FRapidIterationOverrideIndex* OverrideIndex = nullptr)
    {
        // Sort by FName key collected during emit so we don't TryGetStringField twice per comparison.
        TArray<TPair<FName, TSharedPtr<FJsonValue>>> Pairs;
        for (const FNiagaraVariableWithOffset& Variable : Store.ReadParameterVariables())
        {
            const FString VarName = Variable.GetName().ToString();
            // Optional case-insensitive substring narrowing so a single-parameter
            // readback stays inline instead of serializing the whole store.
            if (!NameFilter.IsEmpty() && !VarName.Contains(NameFilter, ESearchCase::IgnoreCase))
            {
                continue;
            }
            TSharedPtr<FJsonObject> Entry = MakeObject();
            Entry->SetStringField(TEXT("scope"), Scope);
            Entry->SetStringField(TEXT("name"), VarName);
            Entry->SetObjectField(TEXT("type"), BuildVariableTypeJson(Variable.GetType()));
            Entry->SetNumberField(TEXT("offset"), Variable.Offset);
            Entry->SetField(TEXT("value"), BuildParameterValueJson(Store, Variable));
            // The `value` above is what this store holds; it is not necessarily what drives the
            // input. A module input can also carry a graph override pin, which this store cannot
            // see and which the stack aspect reports as moduleInputs[].valueMode. Naming the pin
            // here is what stops the two aspects reading as a contradiction.
            if (const FRapidIterationOverrideMark* Mark = OverrideIndex ? OverrideIndex->Find(Variable.GetName()) : nullptr)
            {
                Entry->SetBoolField(TEXT("overridden"), true);
                TSharedPtr<FJsonObject> Override = MakeObject();
                Override->SetStringField(TEXT("valueMode"), Mark->ValueMode);
                if (!Mark->Value.IsEmpty())
                {
                    Override->SetStringField(TEXT("value"), Mark->Value);
                }
                if (!Mark->Emitter.IsEmpty())
                {
                    Override->SetStringField(TEXT("emitter"), Mark->Emitter);
                }
                Override->SetStringField(TEXT("module"), Mark->Module);
                Override->SetStringField(TEXT("input"), Mark->Input);
                Entry->SetObjectField(TEXT("override"), Override);
            }
            Pairs.Add(TPair<FName, TSharedPtr<FJsonValue>>(Variable.GetName(), MakeObjectValue(Entry)));
        }
        Pairs.Sort([](const TPair<FName, TSharedPtr<FJsonValue>>& A, const TPair<FName, TSharedPtr<FJsonValue>>& B)
        {
            return A.Key.LexicalLess(B.Key);
        });
        TArray<TSharedPtr<FJsonValue>> Result;
        Result.Reserve(Pairs.Num());
        for (TPair<FName, TSharedPtr<FJsonValue>>& Pair : Pairs)
        {
            Result.Add(MoveTemp(Pair.Value));
        }
        return Result;
    }

    TSharedPtr<FJsonObject> BuildRendererJson(const UNiagaraRendererProperties* Renderer, int32 Index)
    {
        TSharedPtr<FJsonObject> Obj = MakeObject();
        Obj->SetNumberField(TEXT("index"), Index);
        if (!Renderer)
        {
            Obj->SetBoolField(TEXT("present"), false);
            return Obj;
        }

        Obj->SetBoolField(TEXT("present"), true);
        Obj->SetStringField(TEXT("name"), Renderer->GetName());
        Obj->SetStringField(TEXT("path"), Renderer->GetPathName());
        Obj->SetStringField(TEXT("class"), Renderer->GetClass()->GetPathName());
        Obj->SetBoolField(TEXT("enabled"), Renderer->GetIsEnabled());
        Obj->SetObjectField(TEXT("properties"), BuildSparsePropertyDiffJson(
            const_cast<UNiagaraRendererProperties*>(Renderer),
            Renderer->GetClass()->GetDefaultObject()));
        return Obj;
    }

    TArray<TSharedPtr<FJsonValue>> BuildRendererArray(const FVersionedNiagaraEmitterData* EmitterData)
    {
        TArray<TSharedPtr<FJsonValue>> Result;
        if (!EmitterData)
        {
            return Result;
        }

        const TArray<UNiagaraRendererProperties*>& Renderers = EmitterData->GetRenderers();
        for (int32 Index = 0; Index < Renderers.Num(); ++Index)
        {
            Result.Add(MakeObjectValue(BuildRendererJson(Renderers[Index], Index)));
        }
        return Result;
    }

    TSharedPtr<FJsonObject> BuildSimulationStageJson(const UNiagaraSimulationStageBase* Stage, int32 Index)
    {
        TSharedPtr<FJsonObject> Obj = MakeObject();
        Obj->SetNumberField(TEXT("index"), Index);
        if (!Stage)
        {
            Obj->SetBoolField(TEXT("present"), false);
            return Obj;
        }

        Obj->SetBoolField(TEXT("present"), true);
        Obj->SetStringField(TEXT("name"), Stage->GetName());
        Obj->SetStringField(TEXT("path"), Stage->GetPathName());
        Obj->SetStringField(TEXT("class"), Stage->GetClass()->GetPathName());
        Obj->SetObjectField(TEXT("properties"), BuildSparsePropertyDiffJson(
            const_cast<UNiagaraSimulationStageBase*>(Stage),
            Stage->GetClass()->GetDefaultObject()));
        return Obj;
    }

    TArray<TSharedPtr<FJsonValue>> BuildSimulationStageArray(const FVersionedNiagaraEmitterData* EmitterData)
    {
        TArray<TSharedPtr<FJsonValue>> Result;
        if (!EmitterData)
        {
            return Result;
        }

        const TArray<UNiagaraSimulationStageBase*>& Stages = EmitterData->GetSimulationStages();
        for (int32 Index = 0; Index < Stages.Num(); ++Index)
        {
            Result.Add(MakeObjectValue(BuildSimulationStageJson(Stages[Index], Index)));
        }
        return Result;
    }

    // The inheritance link, which is the difference between an emitter that tracks the asset it
    // came from and a frozen copy of it - and, when it IS inherited, whether the copy is still up
    // to date with that asset.
    //
    // Nothing published this before, which is what made B-niagara-add-emitter-snapshots-emitter-
    // silently invisible: a system holding a stale or snapshot copy read identically to one
    // holding the current emitter in every readback, compiled clean, saved clean, and validated
    // strict-clean. `synchronized` is the engine's own change-id comparison against
    // VersionedParentAtLastMerge (FVersionedNiagaraEmitterData::IsSynchronizedWithParent), so it
    // is the same measurement niagara.validate's EMITTER_PARENT_STALE and niagara.refresh_emitter
    // report - the three cannot disagree.
    TSharedPtr<FJsonObject> BuildEmitterParentJson(const FVersionedNiagaraEmitterData* EmitterData)
    {
        TSharedPtr<FJsonObject> Obj = MakeObject();
        const FVersionedNiagaraEmitter ParentRef = EmitterData ? EmitterData->GetParent() : FVersionedNiagaraEmitter();
        const bool bInherited = ParentRef.Emitter != nullptr;
        Obj->SetBoolField(TEXT("inherited"), bInherited);
        if (bInherited)
        {
            Obj->SetStringField(TEXT("path"), GetObjectPathSafe(ParentRef.Emitter));
            Obj->SetStringField(TEXT("version"), ParentRef.Version.ToString());
            Obj->SetBoolField(TEXT("synchronized"), EmitterData->IsSynchronizedWithParent());
            Obj->SetStringField(TEXT("parentAtLastMergePath"), GetObjectPathSafe(EmitterData->GetParentAtLastMerge().Emitter));
        }
        return Obj;
    }

    TSharedPtr<FJsonObject> BuildEmitterDataJson(const FVersionedNiagaraEmitterData* EmitterData)
    {
        TSharedPtr<FJsonObject> Obj = MakeObject();
        if (!EmitterData)
        {
            Obj->SetBoolField(TEXT("present"), false);
            return Obj;
        }

        Obj->SetBoolField(TEXT("present"), true);
        Obj->SetObjectField(TEXT("version"), BuildAssetVersionJson(EmitterData->Version));
        Obj->SetBoolField(TEXT("deprecated"), EmitterData->bDeprecated);
        Obj->SetStringField(TEXT("deprecationMessage"), EmitterData->DeprecationMessage.ToString());
        Obj->SetBoolField(TEXT("localSpace"), EmitterData->bLocalSpace);
        Obj->SetBoolField(TEXT("determinism"), EmitterData->bDeterminism);
        Obj->SetNumberField(TEXT("randomSeed"), EmitterData->RandomSeed);
        Obj->SetStringField(TEXT("simTarget"), EnumToString(EmitterData->SimTarget));
        Obj->SetStringField(TEXT("calculateBoundsMode"), EnumToString(EmitterData->CalculateBoundsMode));
        Obj->SetObjectField(TEXT("fixedBounds"), BuildBoxJson(EmitterData->FixedBounds));
        Obj->SetBoolField(TEXT("requiresPersistentIds"), EmitterData->RequiresPersistentIDs());
        Obj->SetNumberField(TEXT("maxGpuParticlesSpawnPerFrame"), EmitterData->MaxGPUParticlesSpawnPerFrame);
        Obj->SetNumberField(TEXT("preAllocationCount"), EmitterData->PreAllocationCount);
        Obj->SetArrayField(TEXT("renderers"), BuildRendererArray(EmitterData));
        Obj->SetArrayField(TEXT("simulationStages"), BuildSimulationStageArray(EmitterData));
        Obj->SetObjectField(TEXT("spawnScript"), BuildScriptJson(EmitterData->SpawnScriptProps.Script));
        Obj->SetObjectField(TEXT("updateScript"), BuildScriptJson(EmitterData->UpdateScriptProps.Script));
        Obj->SetObjectField(TEXT("emitterSpawnScript"), BuildScriptJson(EmitterData->EmitterSpawnScriptProps.Script));
        Obj->SetObjectField(TEXT("emitterUpdateScript"), BuildScriptJson(EmitterData->EmitterUpdateScriptProps.Script));
        Obj->SetObjectField(TEXT("parent"), BuildEmitterParentJson(EmitterData));
        return Obj;
    }

    TSharedPtr<FJsonObject> BuildEmitterHandleJson(const FNiagaraEmitterHandle& Handle, int32 Index)
    {
        TSharedPtr<FJsonObject> Obj = MakeObject();
        Obj->SetNumberField(TEXT("index"), Index);
        Obj->SetStringField(TEXT("name"), Handle.GetName().ToString());
        Obj->SetStringField(TEXT("id"), Handle.GetId().ToString());
        Obj->SetStringField(TEXT("idName"), Handle.GetIdName().ToString());
        Obj->SetBoolField(TEXT("enabled"), Handle.GetIsEnabled());
        Obj->SetBoolField(TEXT("valid"), Handle.IsValid());
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 4, 0)
        // FNiagaraEmitterHandle::GetEmitterMode()/ENiagaraEmitterMode are UE 5.4+; omit on 5.3.
        Obj->SetStringField(TEXT("mode"), EnumToString(Handle.GetEmitterMode()));
#endif
        Obj->SetStringField(TEXT("uniqueInstanceName"), Handle.GetUniqueInstanceName());
        Obj->SetBoolField(TEXT("needsRecompile"), Handle.NeedsRecompile());
        const FVersionedNiagaraEmitter Instance = Handle.GetInstance();
        Obj->SetStringField(TEXT("emitterPath"), GetObjectPathSafe(Instance.Emitter));
        if (Instance.Emitter)
        {
            Obj->SetStringField(TEXT("uniqueEmitterName"), Instance.Emitter->GetUniqueEmitterName());
        }
        Obj->SetObjectField(TEXT("versionedEmitterData"), BuildEmitterDataJson(Handle.GetEmitterData()));
        return Obj;
    }

    TArray<TSharedPtr<FJsonValue>> BuildEmitterHandlesArray(const UNiagaraSystem* System)
    {
        TArray<TSharedPtr<FJsonValue>> Result;
        if (!System)
        {
            return Result;
        }

        const TArray<FNiagaraEmitterHandle>& Handles = System->GetEmitterHandles();
        for (int32 Index = 0; Index < Handles.Num(); ++Index)
        {
            Result.Add(MakeObjectValue(BuildEmitterHandleJson(Handles[Index], Index)));
        }
        return Result;
    }

    TSharedPtr<FJsonObject> BuildEmitterAssetEntryJson(const UNiagaraEmitter* Emitter)
    {
        TSharedPtr<FJsonObject> Obj = MakeObject();
        if (!Emitter)
        {
            Obj->SetBoolField(TEXT("present"), false);
            return Obj;
        }

        Obj->SetBoolField(TEXT("present"), true);
        Obj->SetStringField(TEXT("name"), Emitter->GetName());
        Obj->SetStringField(TEXT("path"), Emitter->GetPathName());
        Obj->SetStringField(TEXT("uniqueEmitterName"), Emitter->GetUniqueEmitterName());
        Obj->SetObjectField(TEXT("exposedVersion"), BuildAssetVersionJson(Emitter->GetExposedVersion()));
        Obj->SetObjectField(TEXT("versionedEmitterData"), BuildEmitterDataJson(Emitter->GetLatestEmitterData()));
        return Obj;
    }

    TSharedPtr<FJsonObject> BuildPinTypeJson(const FEdGraphPinType& PinType)
    {
        TSharedPtr<FJsonObject> Obj = MakeObject();
        Obj->SetStringField(TEXT("category"), PinType.PinCategory.ToString());
        Obj->SetStringField(TEXT("subCategory"), PinType.PinSubCategory.ToString());
        if (PinType.PinSubCategoryObject.IsValid())
        {
            Obj->SetStringField(TEXT("subCategoryObject"), PinType.PinSubCategoryObject->GetPathName());
        }
        Obj->SetBoolField(TEXT("isArray"), PinType.ContainerType == EPinContainerType::Array);
        Obj->SetBoolField(TEXT("isSet"), PinType.ContainerType == EPinContainerType::Set);
        Obj->SetBoolField(TEXT("isMap"), PinType.ContainerType == EPinContainerType::Map);
        return Obj;
    }

    // A Parameter Map Get holds each output parameter's fallback on an unnamed input pin, and
    // records which goes with which in PinOutputToPinDefaultPersistentId — keyed on
    // PersistentGuid, an id no readback publishes, while the pin `id` emitted here is PinId.
    // The two id spaces never meet, so a caller reading the dump cannot tell which anonymous
    // default belongs to which parameter (positional order is not a convention: it is forward
    // on some nodes and reversed on others). Name the output pin each default backs, resolved
    // here where both the node and the map are in hand.
    void AddMapGetDefaultPinOwner(const TSharedPtr<FJsonObject>& Obj, const UEdGraphPin* Pin)
    {
        if (!Obj.IsValid() || !Pin || Pin->Direction != EGPD_Input || !Pin->PersistentGuid.IsValid())
        {
            return;
        }
        // UNiagaraNodeParameterMapGet carries no NIAGARAEDITOR_API and the pairing map is
        // protected, so both are reached by reflection: the class by path, the map as the
        // UPROPERTY it is declared as.
        static const UClass* MapGetClass =
            FindObject<UClass>(nullptr, TEXT("/Script/NiagaraEditor.NiagaraNodeParameterMapGet"));
        const UEdGraphNode* OwningNode = Pin->GetOwningNodeUnchecked();
        if (!MapGetClass || !OwningNode || !OwningNode->IsA(MapGetClass))
        {
            return;
        }
        static const FMapProperty* PairingProperty =
            FindFProperty<FMapProperty>(MapGetClass, TEXT("PinOutputToPinDefaultPersistentId"));
        if (!PairingProperty)
        {
            return;
        }

        for (const TPair<FGuid, FGuid>& Pairing : *PairingProperty->ContainerPtrToValuePtr<TMap<FGuid, FGuid>>(OwningNode))
        {
            if (Pairing.Value != Pin->PersistentGuid)
            {
                continue;
            }
            for (const UEdGraphPin* OutputPin : OwningNode->Pins)
            {
                if (OutputPin && OutputPin->Direction == EGPD_Output && OutputPin->PersistentGuid == Pairing.Key)
                {
                    Obj->SetStringField(TEXT("defaultForOutputPin"), OutputPin->PinName.ToString());
                    Obj->SetStringField(TEXT("defaultForPinId"), OutputPin->PinId.ToString());
                    return;
                }
            }
        }
    }

    TSharedPtr<FJsonObject> BuildPinJson(const UEdGraphPin* Pin)
    {
        TSharedPtr<FJsonObject> Obj = MakeObject();
        if (!Pin)
        {
            Obj->SetBoolField(TEXT("present"), false);
            return Obj;
        }

        Obj->SetBoolField(TEXT("present"), true);
        Obj->SetStringField(TEXT("id"), Pin->PinId.ToString());
        Obj->SetStringField(TEXT("name"), Pin->PinName.ToString());
        Obj->SetStringField(TEXT("displayName"), Pin->GetDisplayName().ToString());
        Obj->SetStringField(TEXT("direction"), Pin->Direction == EGPD_Input ? TEXT("input") : TEXT("output"));
        Obj->SetObjectField(TEXT("type"), BuildPinTypeJson(Pin->PinType));
        // An input pin with an inbound link reads the link; the string left on its DefaultValue
        // is never evaluated. Reporting a non-empty one as `defaultValue` presents a stale
        // literal as the pin's effective value — the readback complement of the write the
        // literal path refuses with MODULE_INPUT_OVERRIDE_LINKED — and this is the aspect the
        // set_module_input docs point at for the live override value. Omit the value and say
        // why, keeping the stored text as rawDefaultValue for restoring or diffing it.
        if (Pin->Direction == EGPD_Input && Pin->LinkedTo.Num() > 0 && !Pin->DefaultValue.IsEmpty())
        {
            Obj->SetStringField(TEXT("rawDefaultValue"), Pin->DefaultValue);
            Obj->SetStringField(TEXT("defaultValueError"), TEXT("MODULE_INPUT_OVERRIDE_LINKED: this pin has an inbound link, so the graph reads the link and never this stored default. Read the effective value from the driving source (stack aspect moduleInputs[].valueMode)."));
        }
        else
        {
            Obj->SetStringField(TEXT("defaultValue"), Pin->DefaultValue);
        }
        Obj->SetStringField(TEXT("defaultObject"), GetObjectPathSafe(Pin->DefaultObject));
        Obj->SetStringField(TEXT("defaultText"), Pin->DefaultTextValue.ToString());
        Obj->SetNumberField(TEXT("linkCount"), Pin->LinkedTo.Num());
        AddMapGetDefaultPinOwner(Obj, Pin);
        return Obj;
    }

    TSharedPtr<FJsonObject> BuildNodeJson(const UEdGraphNode* Node)
    {
        TSharedPtr<FJsonObject> Obj = MakeObject();
        if (!Node)
        {
            Obj->SetBoolField(TEXT("present"), false);
            return Obj;
        }

        Obj->SetBoolField(TEXT("present"), true);
        Obj->SetStringField(TEXT("id"), Node->NodeGuid.ToString());
        Obj->SetStringField(TEXT("name"), Node->GetName());
        Obj->SetStringField(TEXT("title"), Node->GetNodeTitle(ENodeTitleType::ListView).ToString());
        Obj->SetStringField(TEXT("class"), Node->GetClass()->GetPathName());
        Obj->SetNumberField(TEXT("posX"), Node->NodePosX);
        Obj->SetNumberField(TEXT("posY"), Node->NodePosY);

        if (const UNiagaraNodeFunctionCall* FunctionCall = Cast<UNiagaraNodeFunctionCall>(Node))
        {
            Obj->SetStringField(TEXT("functionName"), FunctionCall->GetFunctionName());
            Obj->SetStringField(TEXT("functionScript"), GetObjectPathSafe(FunctionCall->FunctionScript));
            Obj->SetStringField(TEXT("selectedScriptVersion"), FunctionCall->SelectedScriptVersion.ToString());
            Obj->SetStringField(TEXT("functionScriptAssetObjectPath"), FunctionCall->FunctionScriptAssetObjectPath.ToString());
        }
        if (const UNiagaraNodeOutput* OutputNode = Cast<UNiagaraNodeOutput>(Node))
        {
            Obj->SetStringField(TEXT("usage"), EnumToString(OutputNode->GetUsage()));
            Obj->SetStringField(TEXT("usageId"), OutputNode->GetUsageId().ToString());
        }
        if (const UNiagaraNodeInput* InputNode = Cast<UNiagaraNodeInput>(Node))
        {
            Obj->SetStringField(TEXT("inputUsage"), EnumToString(InputNode->Usage));
        }
        if (const UNiagaraNodeStaticSwitch* SwitchNode = Cast<UNiagaraNodeStaticSwitch>(Node))
        {
            Obj->SetStringField(TEXT("inputName"), SwitchNode->InputParameterName.ToString());
            Obj->SetStringField(TEXT("switchType"), EnumToString(SwitchNode->SwitchTypeData.SwitchType));
            Obj->SetStringField(TEXT("enumPath"), GetObjectPathSafe(SwitchNode->SwitchTypeData.Enum));
        }

        TArray<TSharedPtr<FJsonValue>> Pins;
        for (const UEdGraphPin* Pin : Node->Pins)
        {
            Pins.Add(MakeObjectValue(BuildPinJson(Pin)));
        }
        Pins.Sort([](const TSharedPtr<FJsonValue>& A, const TSharedPtr<FJsonValue>& B)
        {
            FString AName;
            FString BName;
            A->AsObject()->TryGetStringField(TEXT("name"), AName);
            B->AsObject()->TryGetStringField(TEXT("name"), BName);
            return AName < BName;
        });
        Obj->SetArrayField(TEXT("pins"), Pins);
        return Obj;
    }

    TSharedPtr<FJsonObject> BuildLinkJson(const UEdGraphPin* FromPin, const UEdGraphPin* ToPin)
    {
        TSharedPtr<FJsonObject> Obj = MakeObject();
        Obj->SetStringField(TEXT("fromNode"), FromPin && FromPin->GetOwningNode() ? FromPin->GetOwningNode()->NodeGuid.ToString() : FString());
        Obj->SetStringField(TEXT("fromPin"), FromPin ? FromPin->PinId.ToString() : FString());
        Obj->SetStringField(TEXT("fromPinName"), FromPin ? FromPin->PinName.ToString() : FString());
        Obj->SetStringField(TEXT("toNode"), ToPin && ToPin->GetOwningNode() ? ToPin->GetOwningNode()->NodeGuid.ToString() : FString());
        Obj->SetStringField(TEXT("toPin"), ToPin ? ToPin->PinId.ToString() : FString());
        Obj->SetStringField(TEXT("toPinName"), ToPin ? ToPin->PinName.ToString() : FString());
        return Obj;
    }

    TSharedPtr<FJsonObject> BuildGraphJson(const FString& OwnerKind, const FString& OwnerName, const FString& ScriptUsage, const UNiagaraGraph* Graph)
    {
        TSharedPtr<FJsonObject> Obj = MakeObject();
        Obj->SetStringField(TEXT("ownerKind"), OwnerKind);
        Obj->SetStringField(TEXT("ownerName"), OwnerName);
        Obj->SetStringField(TEXT("scriptUsage"), ScriptUsage);
        if (!Graph)
        {
            Obj->SetBoolField(TEXT("present"), false);
            return Obj;
        }

        Obj->SetBoolField(TEXT("present"), true);
        Obj->SetStringField(TEXT("name"), Graph->GetName());
        Obj->SetStringField(TEXT("path"), Graph->GetPathName());

        TArray<TSharedPtr<FJsonValue>> Nodes;
        TArray<TSharedPtr<FJsonValue>> Links;
        for (const UEdGraphNode* Node : Graph->Nodes)
        {
            if (!Node)
            {
                continue;
            }
            Nodes.Add(MakeObjectValue(BuildNodeJson(Node)));
            for (const UEdGraphPin* Pin : Node->Pins)
            {
                if (!Pin || Pin->Direction != EGPD_Output)
                {
                    continue;
                }
                for (const UEdGraphPin* LinkedPin : Pin->LinkedTo)
                {
                    Links.Add(MakeObjectValue(BuildLinkJson(Pin, LinkedPin)));
                }
            }
        }

        Nodes.Sort([](const TSharedPtr<FJsonValue>& A, const TSharedPtr<FJsonValue>& B)
        {
            const TSharedPtr<FJsonObject> AObj = A->AsObject();
            const TSharedPtr<FJsonObject> BObj = B->AsObject();
            const double AY = AObj->GetNumberField(TEXT("posY"));
            const double BY = BObj->GetNumberField(TEXT("posY"));
            if (!FMath::IsNearlyEqual(AY, BY))
            {
                return AY < BY;
            }
            FString AId;
            FString BId;
            AObj->TryGetStringField(TEXT("id"), AId);
            BObj->TryGetStringField(TEXT("id"), BId);
            return AId < BId;
        });

        Links.Sort([](const TSharedPtr<FJsonValue>& A, const TSharedPtr<FJsonValue>& B)
        {
            FString AFrom;
            FString BFrom;
            A->AsObject()->TryGetStringField(TEXT("fromPin"), AFrom);
            B->AsObject()->TryGetStringField(TEXT("fromPin"), BFrom);
            if (AFrom != BFrom)
            {
                return AFrom < BFrom;
            }
            FString ATo;
            FString BTo;
            A->AsObject()->TryGetStringField(TEXT("toPin"), ATo);
            B->AsObject()->TryGetStringField(TEXT("toPin"), BTo);
            return ATo < BTo;
        });

        Obj->SetArrayField(TEXT("nodes"), Nodes);
        Obj->SetArrayField(TEXT("links"), Links);
        return Obj;
    }

    void AddScriptGraph(TArray<TSharedPtr<FJsonValue>>& Graphs, const FString& OwnerKind, const FString& OwnerName, const FString& Usage, const UNiagaraScript* Script)
    {
        Graphs.Add(MakeObjectValue(BuildGraphJson(OwnerKind, OwnerName, Usage, GetGraphFromScript(Script))));
    }

    TArray<TSharedPtr<FJsonValue>> BuildSystemGraphArray(const UNiagaraSystem* System)
    {
        TArray<TSharedPtr<FJsonValue>> Graphs;
        if (!System)
        {
            return Graphs;
        }

        AddScriptGraph(Graphs, TEXT("system"), System->GetName(), TEXT("SystemSpawnScript"), System->GetSystemSpawnScript());
        AddScriptGraph(Graphs, TEXT("system"), System->GetName(), TEXT("SystemUpdateScript"), System->GetSystemUpdateScript());

        for (const FNiagaraEmitterHandle& Handle : System->GetEmitterHandles())
        {
            const FVersionedNiagaraEmitterData* EmitterData = Handle.GetEmitterData();
            const FString EmitterName = Handle.GetName().ToString();
            AddScriptGraph(Graphs, TEXT("emitter"), EmitterName, TEXT("EmitterSpawnScript"), EmitterData ? EmitterData->EmitterSpawnScriptProps.Script : nullptr);
            AddScriptGraph(Graphs, TEXT("emitter"), EmitterName, TEXT("EmitterUpdateScript"), EmitterData ? EmitterData->EmitterUpdateScriptProps.Script : nullptr);
            AddScriptGraph(Graphs, TEXT("emitter"), EmitterName, TEXT("ParticleSpawnScript"), EmitterData ? EmitterData->SpawnScriptProps.Script : nullptr);
            AddScriptGraph(Graphs, TEXT("emitter"), EmitterName, TEXT("ParticleUpdateScript"), EmitterData ? EmitterData->UpdateScriptProps.Script : nullptr);
        }

        return Graphs;
    }

    TArray<TSharedPtr<FJsonValue>> BuildEmitterGraphArray(const UNiagaraEmitter* Emitter)
    {
        TArray<TSharedPtr<FJsonValue>> Graphs;
        if (!Emitter)
        {
            return Graphs;
        }

        const FVersionedNiagaraEmitterData* EmitterData = Emitter->GetLatestEmitterData();
        AddScriptGraph(Graphs, TEXT("emitterAsset"), Emitter->GetName(), TEXT("EmitterSpawnScript"), EmitterData ? EmitterData->EmitterSpawnScriptProps.Script : nullptr);
        AddScriptGraph(Graphs, TEXT("emitterAsset"), Emitter->GetName(), TEXT("EmitterUpdateScript"), EmitterData ? EmitterData->EmitterUpdateScriptProps.Script : nullptr);
        AddScriptGraph(Graphs, TEXT("emitterAsset"), Emitter->GetName(), TEXT("ParticleSpawnScript"), EmitterData ? EmitterData->SpawnScriptProps.Script : nullptr);
        AddScriptGraph(Graphs, TEXT("emitterAsset"), Emitter->GetName(), TEXT("ParticleUpdateScript"), EmitterData ? EmitterData->UpdateScriptProps.Script : nullptr);
        return Graphs;
    }

    TArray<TSharedPtr<FJsonValue>> BuildScriptGraphArray(const UNiagaraScript* Script)
    {
        TArray<TSharedPtr<FJsonValue>> Graphs;
        if (!Script)
        {
            return Graphs;
        }

        AddScriptGraph(Graphs, TEXT("scriptAsset"), Script->GetName(), EnumToString(Script->GetUsage()), Script);
        return Graphs;
    }

    TSharedPtr<FJsonObject> BuildStackModuleJson(const UNiagaraNodeFunctionCall* Node, const FString& OwnerKind, const FString& OwnerName, const FString& ScriptUsage, int32 Index)
    {
        TSharedPtr<FJsonObject> Obj = MakeObject();
        Obj->SetStringField(TEXT("ownerKind"), OwnerKind);
        Obj->SetStringField(TEXT("ownerName"), OwnerName);
        // Which stage the module sits in (EmitterSpawnScript / ParticleUpdateScript / ...) — the
        // same vocabulary AddScriptCompileEntry emits and the edit verbs take as scriptUsage.
        // Empty only for a module on no ParameterMap chain, which belongs to no stage.
        Obj->SetStringField(TEXT("scriptUsage"), ScriptUsage);
        Obj->SetNumberField(TEXT("index"), Index);
        if (!Node)
        {
            Obj->SetBoolField(TEXT("present"), false);
            return Obj;
        }

        Obj->SetBoolField(TEXT("present"), true);
        Obj->SetStringField(TEXT("entryId"), Node->NodeGuid.ToString());
        // `entryId` is the raw UEdGraphNode::NodeGuid, and duplicating an emitter copies its
        // graph verbatim — NodeGuids included — so emitters descended from one template carry
        // byte-identical entryIds for their corresponding modules. `entryKey` qualifies the id
        // with this entry's owner, which makes it the form that survives being stored and
        // replayed into a mutation: the edit verbs accept it wherever they accept `entryId`,
        // and refuse it against any other owner (B-niagara-entry-id-not-unique-across-emitters).
        Obj->SetStringField(TEXT("entryKey"), NiagaraEdit::MakeModuleEntryKey(OwnerName, Node->NodeGuid));
        Obj->SetStringField(TEXT("name"), Node->GetFunctionName());
        Obj->SetStringField(TEXT("nodeName"), Node->GetName());
        Obj->SetStringField(TEXT("title"), Node->GetNodeTitle(ENodeTitleType::ListView).ToString());
        Obj->SetStringField(TEXT("functionScript"), GetObjectPathSafe(Node->FunctionScript));
        Obj->SetStringField(TEXT("selectedScriptVersion"), Node->SelectedScriptVersion.ToString());
        Obj->SetNumberField(TEXT("posY"), Node->NodePosY);
        Obj->SetArrayField(TEXT("staticSwitchInputs"), NiagaraDumpBuilder::BuildStaticSwitchInputs(Node));
        Obj->SetArrayField(TEXT("moduleInputs"), NiagaraDumpBuilder::BuildModuleInputsJson(Node));
        return Obj;
    }

    // One emitted stack entry: the module node, the stage it belongs to, and its position
    // within that stage's stack.
    struct FStackModuleEntry
    {
        const UNiagaraNodeFunctionCall* Node = nullptr;
        // EnumToString of the usage of the UNiagaraNodeOutput whose ParameterMap chain reached
        // Node; empty when the node sits on no chain at all.
        FString ScriptUsage;
        // Position within that stage's stack, restarting at 0 per stage.
        int32 Index = 0;
    };

    // Visit order for a graph's output nodes: the pipeline stages in the order they run, then
    // anything else (event handlers, simulation stages). Graph->Nodes order is an authoring
    // artifact, so ranking keeps the emitted array grouped and stable across dumps.
    int32 GetStackStageRank(ENiagaraScriptUsage Usage)
    {
        switch (Usage)
        {
        case ENiagaraScriptUsage::SystemSpawnScript:    return 0;
        case ENiagaraScriptUsage::SystemUpdateScript:   return 1;
        case ENiagaraScriptUsage::EmitterSpawnScript:   return 2;
        case ENiagaraScriptUsage::EmitterUpdateScript:  return 3;
        case ENiagaraScriptUsage::ParticleSpawnScript:  return 4;
        case ENiagaraScriptUsage::ParticleUpdateScript: return 5;
        default:                                        return 6;
        }
    }

    // Orders the graph's module function-call nodes by the ParameterMap execution chain
    // (the order the stack actually runs / move_module edits), not by visual NodePosY.
    // Per output node it delegates to PinWrightNiagara::CollectModuleNodesForOutput — the
    // single canonical stack-execution-order walk that move_module's GetOrderedModuleNodes
    // (NiagaraEditHandler.cpp) also calls, so the readback and the edit path can never
    // disagree on module order. This wrapper adds only the dump-specific concerns: output
    // nodes visited in pipeline-stage order with each entry stamped and re-indexed per
    // stage, a cross-output dedup (a module reached from one output is not re-listed under
    // another) and a NodePosY-ordered tail of any function-call node not reachable on a
    // chain (a disconnected/malformed graph) so the readback still lists every module.
    TArray<FStackModuleEntry> CollectStackModuleNodesInExecutionOrder(const UNiagaraGraph* Graph)
    {
        TArray<FStackModuleEntry> Ordered;
        if (!Graph)
        {
            return Ordered;
        }

        TArray<UNiagaraNodeOutput*> OutputNodes;
        for (UEdGraphNode* GraphNode : Graph->Nodes)
        {
            if (UNiagaraNodeOutput* OutputNode = Cast<UNiagaraNodeOutput>(GraphNode))
            {
                OutputNodes.Add(OutputNode);
            }
        }
        OutputNodes.Sort([](const UNiagaraNodeOutput& A, const UNiagaraNodeOutput& B)
        {
            const int32 RankA = GetStackStageRank(A.GetUsage());
            const int32 RankB = GetStackStageRank(B.GetUsage());
            if (RankA != RankB)
            {
                return RankA < RankB;
            }
            if (A.GetUsage() != B.GetUsage())
            {
                return static_cast<int32>(A.GetUsage()) < static_cast<int32>(B.GetUsage());
            }
            return A.GetUsageId().ToString() < B.GetUsageId().ToString();
        });

        TSet<const UNiagaraNodeFunctionCall*> Seen;
        for (UNiagaraNodeOutput* OutputNode : OutputNodes)
        {
            TArray<UNiagaraNodeFunctionCall*> Chain;
            PinWrightNiagara::CollectModuleNodesForOutput(*OutputNode, Chain);
            const FString ScriptUsage = EnumToString(OutputNode->GetUsage());
            int32 Index = 0;
            for (UNiagaraNodeFunctionCall* ModuleNode : Chain)
            {
                if (!Seen.Contains(ModuleNode))
                {
                    Seen.Add(ModuleNode);
                    Ordered.Add(FStackModuleEntry{ ModuleNode, ScriptUsage, Index++ });
                }
            }
        }

        // Append any module nodes not reached on a ParameterMap chain, in NodePosY order,
        // so a disconnected graph still surfaces all of its modules.
        TArray<const UNiagaraNodeFunctionCall*> Leftover;
        for (const UEdGraphNode* GraphNode : Graph->Nodes)
        {
            if (const UNiagaraNodeFunctionCall* FunctionCall = Cast<UNiagaraNodeFunctionCall>(GraphNode))
            {
                if (!Seen.Contains(FunctionCall))
                {
                    Leftover.Add(FunctionCall);
                }
            }
        }
        Leftover.Sort([](const UNiagaraNodeFunctionCall& A, const UNiagaraNodeFunctionCall& B)
        {
            if (A.NodePosY != B.NodePosY)
            {
                return A.NodePosY < B.NodePosY;
            }
            return A.NodeGuid.ToString() < B.NodeGuid.ToString();
        });
        for (int32 Index = 0; Index < Leftover.Num(); ++Index)
        {
            Ordered.Add(FStackModuleEntry{ Leftover[Index], FString(), Index });
        }
        return Ordered;
    }

    void AddGraphStackModules(TArray<TSharedPtr<FJsonValue>>& Modules, const FString& OwnerKind, const FString& OwnerName, const UNiagaraGraph* Graph)
    {
        for (const FStackModuleEntry& Entry : CollectStackModuleNodesInExecutionOrder(Graph))
        {
            Modules.Add(MakeObjectValue(BuildStackModuleJson(Entry.Node, OwnerKind, OwnerName, Entry.ScriptUsage, Entry.Index)));
        }
    }

    // An owner's scripts all hang off that owner's single UNiagaraScriptSource, so
    // GetGraphFromScript returns the *same* UNiagaraGraph for every one of them, and that graph
    // holds the output nodes of every stage. Walking it once per script therefore emitted each
    // module once per script (4x per emitter, 2x per system) with the copies byte-identical;
    // walk each distinct graph exactly once instead.
    void AddEmitterDataStackModules(TArray<TSharedPtr<FJsonValue>>& Modules, const FString& OwnerKind, const FString& OwnerName, const FVersionedNiagaraEmitterData* EmitterData)
    {
        if (!EmitterData)
        {
            return;
        }

        TArray<const UNiagaraGraph*> Graphs;
        Graphs.AddUnique(GetGraphFromScript(EmitterData->EmitterSpawnScriptProps.Script));
        Graphs.AddUnique(GetGraphFromScript(EmitterData->EmitterUpdateScriptProps.Script));
        Graphs.AddUnique(GetGraphFromScript(EmitterData->SpawnScriptProps.Script));
        Graphs.AddUnique(GetGraphFromScript(EmitterData->UpdateScriptProps.Script));
        for (const UNiagaraGraph* Graph : Graphs)
        {
            AddGraphStackModules(Modules, OwnerKind, OwnerName, Graph);
        }
    }

    TArray<TSharedPtr<FJsonValue>> BuildSystemStackArray(const UNiagaraSystem* System)
    {
        TArray<TSharedPtr<FJsonValue>> Modules;
        if (!System)
        {
            return Modules;
        }

        // Same shared-source reasoning as AddEmitterDataStackModules, for the system's two scripts.
        TArray<const UNiagaraGraph*> SystemGraphs;
        SystemGraphs.AddUnique(GetGraphFromScript(System->GetSystemSpawnScript()));
        SystemGraphs.AddUnique(GetGraphFromScript(System->GetSystemUpdateScript()));
        for (const UNiagaraGraph* Graph : SystemGraphs)
        {
            AddGraphStackModules(Modules, TEXT("system"), System->GetName(), Graph);
        }

        for (const FNiagaraEmitterHandle& Handle : System->GetEmitterHandles())
        {
            AddEmitterDataStackModules(Modules, TEXT("emitter"), Handle.GetName().ToString(), Handle.GetEmitterData());
        }

        return Modules;
    }

    TArray<TSharedPtr<FJsonValue>> BuildEmitterStackArray(const UNiagaraEmitter* Emitter)
    {
        TArray<TSharedPtr<FJsonValue>> Modules;
        if (!Emitter)
        {
            return Modules;
        }

        AddEmitterDataStackModules(Modules, TEXT("emitterAsset"), Emitter->GetName(), Emitter->GetLatestEmitterData());
        return Modules;
    }

    void AddScriptCompileEntry(TArray<TSharedPtr<FJsonValue>>& Scripts, const FString& OwnerKind, const FString& OwnerName, const FString& Usage, const UNiagaraScript* Script)
    {
        TSharedPtr<FJsonObject> Obj = BuildScriptJson(Script);
        Obj->SetStringField(TEXT("ownerKind"), OwnerKind);
        Obj->SetStringField(TEXT("ownerName"), OwnerName);
        Obj->SetStringField(TEXT("scriptUsage"), Usage);
        Scripts.Add(MakeObjectValue(Obj));
    }

    bool ScriptsHaveUninitializedCompileStatus(const TArray<TSharedPtr<FJsonValue>>& Scripts)
    {
        for (const TSharedPtr<FJsonValue>& Value : Scripts)
        {
            const TSharedPtr<FJsonObject> Obj = Value.IsValid() ? Value->AsObject() : nullptr;
            if (!Obj.IsValid())
            {
                continue;
            }
            const TSharedPtr<FJsonValue> StatusField = Obj->TryGetField(TEXT("compileStatus"));
            if (StatusField.IsValid() && StatusField->Type == EJson::Null)
            {
                return true;
            }
        }
        return false;
    }

    void AddGpuIncompatibleModuleIssues(
        TArray<TSharedPtr<FJsonValue>>& Issues,
        const FVersionedNiagaraEmitterData* EmitterData,
        const FString& EmitterName)
    {
        for (const FNiagaraGpuIncompatibleModule& Module : NiagaraDecompileHelpers::CollectGpuIncompatibleModules(EmitterData))
        {
            TSharedPtr<FJsonObject> Issue = MakeObject();
            Issue->SetStringField(TEXT("severity"), TEXT("error"));
            Issue->SetStringField(TEXT("code"), TEXT("GPU_INCOMPATIBLE_MODULE"));
            Issue->SetStringField(TEXT("emitter"), EmitterName);
            Issue->SetStringField(TEXT("stack"), Module.StackName);
            Issue->SetStringField(TEXT("module"), Module.ModuleName);
            Issue->SetStringField(TEXT("node"), Module.NodeName);
            Issue->SetStringField(TEXT("message"), FString::Printf(
                TEXT("GPU emitter contains module '%s' whose Niagara function signature does not support GPU execution."),
                *Module.ModuleName));
            Issues.Add(MakeObjectValue(Issue));
        }
    }

    // The four fixed emitter scripts plus every optional one the emitter actually owns. The
    // optional set matters because it is exactly what FVersionedNiagaraEmitterData::IsValidInternal
    // consults: an event-handler script at NCS_Error makes the engine refuse to instance the
    // system just as a particle script does, and a compile-scripts array that stopped at
    // spawn+update could never report it (B-niagara-validate-green-while-scripts-ncs-error).
    // GPUComputeScript is listed only for a GPU emitter, matching the same engine check - a CPU
    // emitter carries the object but never compiles it, so listing it would report a permanent
    // NCS_Unknown for a script nothing runs.
    void AddEmitterCompileEntries(
        TArray<TSharedPtr<FJsonValue>>& Scripts,
        const TCHAR* OwnerKind,
        const FString& OwnerName,
        const FVersionedNiagaraEmitterData* EmitterData)
    {
        AddScriptCompileEntry(Scripts, OwnerKind, OwnerName, TEXT("EmitterSpawnScript"), EmitterData ? EmitterData->EmitterSpawnScriptProps.Script : nullptr);
        AddScriptCompileEntry(Scripts, OwnerKind, OwnerName, TEXT("EmitterUpdateScript"), EmitterData ? EmitterData->EmitterUpdateScriptProps.Script : nullptr);
        AddScriptCompileEntry(Scripts, OwnerKind, OwnerName, TEXT("ParticleSpawnScript"), EmitterData ? EmitterData->SpawnScriptProps.Script : nullptr);
        AddScriptCompileEntry(Scripts, OwnerKind, OwnerName, TEXT("ParticleUpdateScript"), EmitterData ? EmitterData->UpdateScriptProps.Script : nullptr);
        if (!EmitterData)
        {
            return;
        }

        for (const FNiagaraEventScriptProperties& Handler : EmitterData->GetEventHandlers())
        {
            if (Handler.Script)
            {
                AddScriptCompileEntry(
                    Scripts,
                    OwnerKind,
                    OwnerName,
                    FString::Printf(TEXT("ParticleEventScript:%s"), *Handler.SourceEventName.ToString()),
                    Handler.Script);
            }
        }
        for (const UNiagaraSimulationStageBase* Stage : EmitterData->GetSimulationStages())
        {
            if (Stage && Stage->Script)
            {
                AddScriptCompileEntry(
                    Scripts,
                    OwnerKind,
                    OwnerName,
                    FString::Printf(TEXT("ParticleSimulationStageScript:%s"), *Stage->GetName()),
                    Stage->Script);
            }
        }
        if (EmitterData->SimTarget == ENiagaraSimTarget::GPUComputeSim && EmitterData->GetGPUComputeScript())
        {
            AddScriptCompileEntry(Scripts, OwnerKind, OwnerName, TEXT("ParticleGPUComputeScript"), EmitterData->GetGPUComputeScript());
        }
    }

    TArray<TSharedPtr<FJsonValue>> BuildSystemCompileScriptArray(const UNiagaraSystem* System)
    {
        TArray<TSharedPtr<FJsonValue>> Scripts;
        if (!System)
        {
            return Scripts;
        }

        AddScriptCompileEntry(Scripts, TEXT("system"), System->GetName(), TEXT("SystemSpawnScript"), System->GetSystemSpawnScript());
        AddScriptCompileEntry(Scripts, TEXT("system"), System->GetName(), TEXT("SystemUpdateScript"), System->GetSystemUpdateScript());
        for (const FNiagaraEmitterHandle& Handle : System->GetEmitterHandles())
        {
            AddEmitterCompileEntries(Scripts, TEXT("emitter"), Handle.GetName().ToString(), Handle.GetEmitterData());
        }
        return Scripts;
    }

    TArray<TSharedPtr<FJsonValue>> BuildEmitterCompileScriptArray(const UNiagaraEmitter* Emitter)
    {
        TArray<TSharedPtr<FJsonValue>> Scripts;
        if (!Emitter)
        {
            return Scripts;
        }

        AddEmitterCompileEntries(Scripts, TEXT("emitterAsset"), Emitter->GetName(), Emitter->GetLatestEmitterData());
        return Scripts;
    }

    TArray<TSharedPtr<FJsonValue>> BuildScriptCompileScriptArray(const UNiagaraScript* Script)
    {
        TArray<TSharedPtr<FJsonValue>> Scripts;
        if (!Script)
        {
            return Scripts;
        }

        AddScriptCompileEntry(Scripts, TEXT("scriptAsset"), Script->GetName(), EnumToString(Script->GetUsage()), Script);
        return Scripts;
    }

    TArray<TSharedPtr<FJsonValue>> MakeAuthoredScriptArray(TArray<TSharedPtr<FJsonValue>> Scripts)
    {
        for (const TSharedPtr<FJsonValue>& Value : Scripts)
        {
            if (const TSharedPtr<FJsonObject> ScriptObject = Value.IsValid() ? Value->AsObject() : nullptr)
            {
                ScriptObject->RemoveField(TEXT("compileStatus"));
                // Same reason compileStatus goes: the authored aspect describes what the asset
                // declares, not what the last compile in this session produced.
                ScriptObject->RemoveField(TEXT("compileErrors"));
            }
        }
        return Scripts;
    }

    TArray<TSharedPtr<FJsonValue>> BuildSystemAuthoredIssues(const UNiagaraSystem* System)
    {
        TArray<TSharedPtr<FJsonValue>> Issues;
        if (!System)
        {
            return Issues;
        }
        if (System->GetEmitterHandles().Num() == 0)
        {
            TSharedPtr<FJsonObject> Issue = MakeObject();
            Issue->SetStringField(TEXT("severity"), TEXT("warning"));
            Issue->SetStringField(TEXT("code"), TEXT("NO_EMITTERS"));
            Issue->SetStringField(TEXT("message"), TEXT("Niagara system has no emitter handles."));
            Issues.Add(MakeObjectValue(Issue));
        }
        for (const FNiagaraEmitterHandle& Handle : System->GetEmitterHandles())
        {
            const FVersionedNiagaraEmitterData* EmitterData = Handle.GetEmitterData();
            if (!EmitterData)
            {
                TSharedPtr<FJsonObject> Issue = MakeObject();
                Issue->SetStringField(TEXT("severity"), TEXT("error"));
                Issue->SetStringField(TEXT("code"), TEXT("EMITTER_DATA_MISSING"));
                Issue->SetStringField(TEXT("emitter"), Handle.GetName().ToString());
                Issue->SetStringField(TEXT("message"), TEXT("Emitter handle has no versioned emitter data."));
                Issues.Add(MakeObjectValue(Issue));
                continue;
            }
            if (EmitterData->GetRenderers().Num() == 0)
            {
                TSharedPtr<FJsonObject> Issue = MakeObject();
                Issue->SetStringField(TEXT("severity"), TEXT("warning"));
                Issue->SetStringField(TEXT("code"), TEXT("NO_RENDERERS"));
                Issue->SetStringField(TEXT("emitter"), Handle.GetName().ToString());
                Issue->SetStringField(TEXT("message"), TEXT("Emitter has no renderers."));
                Issues.Add(MakeObjectValue(Issue));
            }
            AddGpuIncompatibleModuleIssues(Issues, EmitterData, Handle.GetName().ToString());
        }
        return Issues;
    }

    TArray<TSharedPtr<FJsonValue>> BuildEmitterAuthoredIssues(const UNiagaraEmitter* Emitter)
    {
        TArray<TSharedPtr<FJsonValue>> Issues;
        const FVersionedNiagaraEmitterData* EmitterData = Emitter ? Emitter->GetLatestEmitterData() : nullptr;
        if (!EmitterData)
        {
            TSharedPtr<FJsonObject> Issue = MakeObject();
            Issue->SetStringField(TEXT("severity"), TEXT("error"));
            Issue->SetStringField(TEXT("code"), TEXT("EMITTER_DATA_MISSING"));
            Issue->SetStringField(TEXT("message"), TEXT("Emitter asset has no latest emitter data."));
            Issues.Add(MakeObjectValue(Issue));
            return Issues;
        }
        if (EmitterData->GetRenderers().Num() == 0)
        {
            TSharedPtr<FJsonObject> Issue = MakeObject();
            Issue->SetStringField(TEXT("severity"), TEXT("warning"));
            Issue->SetStringField(TEXT("code"), TEXT("NO_RENDERERS"));
            Issue->SetStringField(TEXT("message"), TEXT("Emitter has no renderers."));
            Issues.Add(MakeObjectValue(Issue));
        }
        AddGpuIncompatibleModuleIssues(Issues, EmitterData, Emitter->GetName());
        return Issues;
    }

    TArray<TSharedPtr<FJsonValue>> BuildScriptAuthoredIssues(const UNiagaraScript* Script)
    {
        TArray<TSharedPtr<FJsonValue>> Issues;
        if (!Script)
        {
            TSharedPtr<FJsonObject> Issue = MakeObject();
            Issue->SetStringField(TEXT("severity"), TEXT("error"));
            Issue->SetStringField(TEXT("code"), TEXT("SCRIPT_MISSING"));
            Issue->SetStringField(TEXT("message"), TEXT("Niagara script asset is missing."));
            Issues.Add(MakeObjectValue(Issue));
        }
        return Issues;
    }
}

namespace NiagaraDumpBuilder
{
    TArray<TSharedPtr<FJsonValue>> BuildStaticSwitchInputs(const UNiagaraNodeFunctionCall* Node)
    {
        TArray<TSharedPtr<FJsonValue>> Result;
        if (!Node)
        {
            return Result;
        }

        // Static switches resolve through the caller-pin DefaultValue, not RapidIterationParameters.
        UNiagaraGraph* CalledGraph = Node->GetCalledGraph();
        if (!CalledGraph)
        {
            return Result;
        }

        // Build pin lookup once: scanning Node->Pins per switch is O(N*K).
        TMap<FName, const UEdGraphPin*> PinByName;
        PinByName.Reserve(Node->Pins.Num());
        for (const UEdGraphPin* Pin : Node->Pins)
        {
            if (Pin)
            {
                PinByName.Add(Pin->PinName, Pin);
            }
        }

        // Collect (name, value) pairs first to avoid double-extracting strings during sort.
        TArray<TPair<FName, TSharedPtr<FJsonValue>>> Pairs;

        for (const TObjectPtr<UEdGraphNode>& GraphNode : CalledGraph->Nodes)
        {
            const UNiagaraNodeStaticSwitch* SwitchNode = Cast<UNiagaraNodeStaticSwitch>(GraphNode);
            if (!SwitchNode)
            {
                continue;
            }

            TSharedPtr<FJsonObject> Entry = MakeObject();
            const FName InputFName = SwitchNode->InputParameterName;
            Entry->SetStringField(TEXT("name"), InputFName.ToString());
            Entry->SetStringField(TEXT("type"), EnumToString(SwitchNode->SwitchTypeData.SwitchType));
            const UEnum* EnumClass = SwitchNode->SwitchTypeData.Enum;
            if (EnumClass)
            {
                Entry->SetStringField(TEXT("enumPath"), EnumClass->GetPathName());
                // The index <-> label table `value` above is expressed in, and the one
                // niagara.set_static_switch takes. Without it here the integer that selects a
                // branch is not derivable from any read: the authored entry names of the
                // user-defined enums Niagara's stock modules use are NewEnumeratorN, and their
                // order is a permutation of the order of the labels the editor shows.
                Entry->SetArrayField(TEXT("enumOptions"), NiagaraStaticSwitch::MakeEnumOptionsJson(EnumClass));
            }

            // Reimplements UNiagaraGraph::GetStaticSwitchDefaultValue — that method exists in
            // NiagaraEditor's UCLASS(MinimalAPI) class without NIAGARAEDITOR_API decoration.
            // VariableToScriptVariable is a private field; the by-name GetScriptVariable
            // overload is NIAGARAEDITOR_API-exported and resolves to the same script variable
            // for static-switch inputs (which are uniquely keyed by parameter name).
            TOptional<int32> DeclaredDefault;
            if (const UNiagaraScriptVariable* FoundScriptVariable = CalledGraph->GetScriptVariable(InputFName))
            {
                DeclaredDefault = FoundScriptVariable->GetStaticSwitchDefaultValue();
            }
            if (DeclaredDefault.IsSet())
            {
                Entry->SetNumberField(TEXT("defaultValue"), DeclaredDefault.GetValue());
            }

            const UEdGraphPin* const* CallerPinPtr = PinByName.Find(InputFName);
            const UEdGraphPin* CallerPin = CallerPinPtr ? *CallerPinPtr : nullptr;
            const bool bHasOverride = CallerPin != nullptr && !CallerPin->DefaultValue.IsEmpty();

            if (bHasOverride)
            {
                // An override this build cannot decode gets no `value` at all. Writing the
                // declared default here while still stamping source:"override" asserted two
                // things that cannot both hold; `rawValue` plus `valueError` say what is
                // actually stored on the pin and why it did not resolve.
                TSharedPtr<FJsonValue> ValueJson;
                FString DecodeError;
                if (NiagaraStaticSwitch::DecodePinDefault(CallerPin->DefaultValue, SwitchNode->SwitchTypeData.SwitchType, EnumClass, ValueJson, &DecodeError))
                {
                    Entry->SetField(TEXT("value"), ValueJson);
                }
                else
                {
                    Entry->SetStringField(TEXT("rawValue"), CallerPin->DefaultValue);
                    Entry->SetStringField(TEXT("valueError"), DecodeError);
                }
                Entry->SetStringField(TEXT("source"), TEXT("override"));
            }
            else
            {
                Entry->SetNumberField(TEXT("value"), DeclaredDefault.Get(0));
                Entry->SetStringField(TEXT("source"), TEXT("default"));
            }

            Pairs.Add(TPair<FName, TSharedPtr<FJsonValue>>(InputFName, MakeObjectValue(Entry)));
        }

        Pairs.Sort([](const TPair<FName, TSharedPtr<FJsonValue>>& A, const TPair<FName, TSharedPtr<FJsonValue>>& B)
        {
            return A.Key.LexicalLess(B.Key);
        });
        Result.Reserve(Pairs.Num());
        for (TPair<FName, TSharedPtr<FJsonValue>>& Pair : Pairs)
        {
            Result.Add(MoveTemp(Pair.Value));
        }
        return Result;
    }

    TArray<TSharedPtr<FJsonValue>> BuildModuleInputsJson(const UNiagaraNodeFunctionCall* Node)
    {
        TArray<TSharedPtr<FJsonValue>> Result;
        if (!Node || !Node->FunctionScript)
        {
            return Result;
        }

        // The declared input schema (name + Niagara type) comes from the module's stack
        // inputs (the "Module." namespace reads the stack UI shows, e.g. SpawnRate); the
        // current per-input value mode comes from the placed module's override node.
        // Together they give agents the typed input list they otherwise reconstruct by
        // parsing NIR text or by trial-and-error against niagara.set_module_input.
        TArray<FNiagaraVariable> DeclaredInputs;
        NiagaraEdit::EnumerateModuleStackInputs(*Node, DeclaredInputs);

        TMap<FName, NiagaraEdit::FModuleInputBindingInfo> Bindings;
        NiagaraEdit::ClassifyModuleInputBindings(*const_cast<UNiagaraNodeFunctionCall*>(Node), Bindings);

        // A static switch can strand an input whose override pin is written and correct: the
        // compiler takes the other branch and the value is dead code. The gate is one graph hop
        // from the same reads this builder already describes, so report it here rather than
        // leaving a readback to present a dead override as configuration.
        TMap<FName, NiagaraEdit::FModuleInputGate> GatedInputs;
        NiagaraEdit::ClassifyModuleInputReachability(*Node, DeclaredInputs, GatedInputs);

        const UNiagaraGraph* ModuleGraph = const_cast<UNiagaraNodeFunctionCall*>(Node)->GetCalledGraph();

        // Emit sorted by name so the dumped schema is stable run-to-run (cache/diff friendly),
        // matching BuildStaticSwitchInputs.
        TArray<TPair<FName, TSharedPtr<FJsonValue>>> Pairs;
        Pairs.Reserve(DeclaredInputs.Num());
        TSet<FName> SeenNames;
        for (const FNiagaraVariable& Input : DeclaredInputs)
        {
            // Module inputs are declared in the "Module." namespace; report the short
            // user-facing name (the same token niagara.set_module_input's inputName takes and
            // the stack UI shows), which is also how ClassifyModuleInputBindings keys its
            // per-input value modes (FNiagaraParameterHandle::GetName()).
            const FName InputName = FNiagaraParameterHandle(Input.GetName()).GetName();
            if (SeenNames.Contains(InputName))
            {
                continue;
            }
            SeenNames.Add(InputName);
            const FNiagaraTypeDefinition& InputType = Input.GetType();
            TSharedPtr<FJsonObject> Entry = NiagaraJsonHelpers::MakeObject();
            Entry->SetStringField(TEXT("name"), InputName.ToString());
            Entry->SetStringField(TEXT("type"), InputType.GetName());
            Entry->SetObjectField(TEXT("typeInfo"), NiagaraJsonHelpers::BuildTypeModel(InputType));

            // Enumerate the enum's selectable option names so callers know the valid
            // choices for an enum input (BuildTypeModel reports only the enum path).
            if (const UEnum* TypeEnum = InputType.GetEnum())
            {
                TArray<TSharedPtr<FJsonValue>> Options;
                const int32 NumEnums = TypeEnum->NumEnums();
                for (int32 EnumIndex = 0; EnumIndex < NumEnums; ++EnumIndex)
                {
                    const FString OptionName = TypeEnum->GetNameStringByIndex(EnumIndex);
                    if (OptionName.IsEmpty() || OptionName.EndsWith(TEXT("_MAX")))
                    {
                        continue;
                    }
                    Options.Add(NiagaraJsonHelpers::MakeStringValue(OptionName));
                }
                Entry->SetArrayField(TEXT("enumOptions"), Options);
            }

            if (const NiagaraEdit::FModuleInputBindingInfo* Binding = Bindings.Find(InputName))
            {
                Entry->SetStringField(TEXT("valueMode"), Binding->ValueMode);
                if (Binding->ValueMode == TEXT("local"))
                {
                    Entry->SetStringField(TEXT("value"), Binding->LiteralValue);
                }
                else if (Binding->ValueMode == TEXT("linked"))
                {
                    Entry->SetStringField(TEXT("linkedParameter"), Binding->LinkedParameter);
                }
                else if (Binding->ValueMode == TEXT("dynamicInput"))
                {
                    Entry->SetStringField(TEXT("dynamicInput"), Binding->DynamicInputScript);
                }
            }
            else
            {
                // No override entry — the input reads its script-declared default. `default`
                // alone says only that nobody overrode it; publish what the module will
                // therefore use, so an unset input's effective configuration is readable
                // without a hand-decoded walk of the script graph's parameter metadata.
                Entry->SetStringField(TEXT("valueMode"), TEXT("default"));
                NiagaraEdit::FModuleInputDefaultInfo DefaultInfo;
                if (NiagaraEdit::ResolveModuleInputDefault(ModuleGraph, Input, DefaultInfo))
                {
                    Entry->SetStringField(TEXT("defaultMode"), DefaultInfo.DefaultMode);
                    if (!DefaultInfo.DefaultValue.IsEmpty())
                    {
                        Entry->SetStringField(TEXT("defaultValue"), DefaultInfo.DefaultValue);
                    }
                    if (!DefaultInfo.DefaultBinding.IsEmpty())
                    {
                        Entry->SetStringField(TEXT("defaultBinding"), DefaultInfo.DefaultBinding);
                    }
                }
            }

            const NiagaraEdit::FModuleInputGate* Gate = GatedInputs.Find(InputName);
            Entry->SetBoolField(TEXT("reachable"), Gate == nullptr);
            if (Gate)
            {
                Entry->SetObjectField(TEXT("gatedBy"), NiagaraEdit::MakeGatedByJson(*Gate));
            }

            Pairs.Add(TPair<FName, TSharedPtr<FJsonValue>>(InputName, NiagaraJsonHelpers::MakeObjectValue(Entry)));
        }

        Pairs.Sort([](const TPair<FName, TSharedPtr<FJsonValue>>& A, const TPair<FName, TSharedPtr<FJsonValue>>& B)
        {
            return A.Key.LexicalLess(B.Key);
        });
        Result.Reserve(Pairs.Num());
        for (TPair<FName, TSharedPtr<FJsonValue>>& Pair : Pairs)
        {
            Result.Add(MoveTemp(Pair.Value));
        }
        return Result;
    }

    TSharedPtr<FJsonObject> BuildSystemScalabilityModel(const UNiagaraSystem* System)
    {
        TSharedPtr<FJsonObject> Obj = MakeObject();
        if (!System)
        {
            Obj->SetBoolField(TEXT("present"), false);
            return Obj;
        }

        Obj->SetBoolField(TEXT("present"), true);
        Obj->SetBoolField(TEXT("bAllowScalabilityForLocalPlayerFX"), System->AllowScalabilityForLocalPlayerFX());
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 4, 0)
        // UNiagaraSystem::GetScalabilityPlatformSet() is UE 5.4+; omit the platforms object on 5.3.
        Obj->SetObjectField(TEXT("platforms"), BuildPlatformSetJson(System->GetScalabilityPlatformSet()));
#endif

        TArray<TSharedPtr<FJsonValue>> Entries;
        // GetScalabilityOverrides has no const overload in UE 5.6.
        const FNiagaraSystemScalabilityOverrides& Container = const_cast<UNiagaraSystem*>(System)->GetScalabilityOverrides();
        Entries.Reserve(Container.Overrides.Num());
        for (const FNiagaraSystemScalabilityOverride& Override : Container.Overrides)
        {
            Entries.Add(MakeObjectValue(BuildSystemScalabilityOverrideJson(Override)));
        }
        Obj->SetArrayField(TEXT("systemScalability"), Entries);
        return Obj;
    }

    TSharedPtr<FJsonObject> BuildEmitterScalabilityModel(const FVersionedNiagaraEmitterData* EmitterData)
    {
        TSharedPtr<FJsonObject> Obj = MakeObject();
        if (!EmitterData)
        {
            Obj->SetBoolField(TEXT("present"), false);
            return Obj;
        }

        Obj->SetBoolField(TEXT("present"), true);
        Obj->SetObjectField(TEXT("platforms"), BuildPlatformSetJson(EmitterData->Platforms));

        TArray<TSharedPtr<FJsonValue>> Entries;
        Entries.Reserve(EmitterData->ScalabilityOverrides.Overrides.Num());
        for (const FNiagaraEmitterScalabilityOverride& Override : EmitterData->ScalabilityOverrides.Overrides)
        {
            Entries.Add(MakeObjectValue(BuildEmitterScalabilityOverrideJson(Override)));
        }
        Obj->SetArrayField(TEXT("emitterScalability"), Entries);
        return Obj;
    }

    TSharedPtr<FJsonObject> BuildSystemJson(const UNiagaraSystem* System)
    {
        TSharedPtr<FJsonObject> Obj = MakeObject();
        if (!System)
        {
            Obj->SetBoolField(TEXT("present"), false);
            return Obj;
        }

        Obj->SetBoolField(TEXT("present"), true);
        Obj->SetStringField(TEXT("assetKind"), TEXT("NiagaraSystem"));
        Obj->SetStringField(TEXT("name"), System->GetName());
        Obj->SetStringField(TEXT("path"), System->GetPathName());
        Obj->SetNumberField(TEXT("emitterCount"), System->GetEmitterHandles().Num());
        Obj->SetBoolField(TEXT("valid"), System->IsValid());
        Obj->SetBoolField(TEXT("readyToRun"), IsSystemReadyToRunWithoutLoading(System));
        Obj->SetBoolField(TEXT("needsWarmup"), System->NeedsWarmup());
        Obj->SetNumberField(TEXT("warmupTime"), System->GetWarmupTime());
        Obj->SetNumberField(TEXT("warmupTickCount"), System->GetWarmupTickCount());
        Obj->SetNumberField(TEXT("warmupTickDelta"), System->GetWarmupTickDelta());
        Obj->SetBoolField(TEXT("fixedTickDelta"), System->HasFixedTickDelta());
        Obj->SetNumberField(TEXT("fixedTickDeltaTime"), System->GetFixedTickDeltaTime());
        Obj->SetBoolField(TEXT("determinism"), System->NeedsDeterminism());
        Obj->SetNumberField(TEXT("randomSeed"), System->GetRandomSeed());
        Obj->SetObjectField(TEXT("fixedBounds"), BuildBoxJson(System->GetFixedBounds()));
        Obj->SetObjectField(TEXT("systemSpawnScript"), BuildScriptJson(System->GetSystemSpawnScript()));
        Obj->SetObjectField(TEXT("systemUpdateScript"), BuildScriptJson(System->GetSystemUpdateScript()));
        Obj->SetObjectField(TEXT("scalability"), BuildSystemScalabilityModel(System));
        Obj->SetArrayField(TEXT("emitterHandles"), BuildEmitterHandlesArray(System));
        return Obj;
    }

    TSharedPtr<FJsonObject> BuildEmittersJson(const UNiagaraSystem* System)
    {
        TSharedPtr<FJsonObject> Obj = MakeObject();
        Obj->SetStringField(TEXT("assetKind"), TEXT("NiagaraSystem"));
        Obj->SetStringField(TEXT("systemPath"), GetObjectPathSafe(System));
        Obj->SetObjectField(TEXT("scalability"), BuildSystemScalabilityModel(System));
        Obj->SetArrayField(TEXT("emitters"), BuildEmitterHandlesArray(System));
        return Obj;
    }

    TSharedPtr<FJsonObject> BuildEmitterAssetJson(const UNiagaraEmitter* Emitter)
    {
        TSharedPtr<FJsonObject> Obj = MakeObject();
        Obj->SetStringField(TEXT("assetKind"), TEXT("NiagaraEmitter"));
        Obj->SetObjectField(TEXT("emitter"), BuildEmitterAssetEntryJson(Emitter));
        Obj->SetObjectField(TEXT("scalability"), BuildEmitterScalabilityModel(Emitter ? Emitter->GetLatestEmitterData() : nullptr));
        return Obj;
    }

    // Text every parameters readback carries, so the caller of the smallest, most natural "did my
    // value stick?" readback is told what this store can and cannot see without having read the
    // wiki page first.
    const TCHAR* const RapidIterationStoreNote =
        TEXT("Rapid-iteration entries report the value stored on the script. An entry marked ")
        TEXT("`overridden` also has a graph override pin on the same module input, and `override` ")
        TEXT("names that pin's mode and value; the two are independent sources and this store does ")
        TEXT("not see the pin. An input a static switch has routed around may still publish its ")
        TEXT("unused constant here while the variant the switch selected has no constant at all, ")
        TEXT("so a missing name is not a missing value — read the stack aspect's ")
        TEXT("moduleInputs[].valueMode alongside this one.");

    // Map every placed module input that carries an override pin to the rapid-iteration constant
    // that would shadow it. Walks the same graphs and uses the same per-input classification as
    // the stack aspect (NiagaraEdit::ClassifyModuleInputBindings), so what one aspect calls the
    // input's value mode and what the other reports as an override cannot drift apart.
    void AddGraphOverrideMarks(
        FRapidIterationOverrideIndex& OutIndex,
        const UNiagaraGraph* Graph,
        const FString& UniqueEmitterName,
        const FString& EmitterLabel)
    {
        if (!Graph)
        {
            return;
        }

        for (UEdGraphNode* GraphNode : Graph->Nodes)
        {
            UNiagaraNodeOutput* OutputNode = Cast<UNiagaraNodeOutput>(GraphNode);
            if (!OutputNode)
            {
                continue;
            }

            TArray<UNiagaraNodeFunctionCall*> Chain;
            PinWrightNiagara::CollectModuleNodesForOutput(*OutputNode, Chain);
            for (UNiagaraNodeFunctionCall* ModuleNode : Chain)
            {
                if (!ModuleNode)
                {
                    continue;
                }
                TMap<FName, NiagaraEdit::FModuleInputBindingInfo> Bindings;
                NiagaraEdit::ClassifyModuleInputBindings(*ModuleNode, Bindings);
                for (const TPair<FName, NiagaraEdit::FModuleInputBindingInfo>& Binding : Bindings)
                {
                    const FNiagaraParameterHandle AliasedHandle =
                        FNiagaraParameterHandle::CreateAliasedModuleParameterHandle(
                            FNiagaraParameterHandle::CreateModuleParameterHandle(Binding.Key),
                            ModuleNode);
                    const FName ConstantName = PinWrightNiagara::MakeRapidIterationConstantName(
                        AliasedHandle.GetParameterHandleString(),
                        UniqueEmitterName,
                        OutputNode->GetUsage());

                    FRapidIterationOverrideMark Mark;
                    Mark.ValueMode = Binding.Value.ValueMode;
                    Mark.Value = Binding.Value.LiteralValue;
                    Mark.Emitter = EmitterLabel;
                    Mark.Module = ModuleNode->GetFunctionName();
                    Mark.Input = Binding.Key.ToString();
                    OutIndex.Add(ConstantName, MoveTemp(Mark));
                }
            }
        }
    }

    void AddEmitterDataOverrideMarks(
        FRapidIterationOverrideIndex& OutIndex,
        const FVersionedNiagaraEmitterData* EmitterData,
        const FString& UniqueEmitterName,
        const FString& EmitterLabel)
    {
        if (!EmitterData)
        {
            return;
        }
        // Same shared-source reasoning as AddEmitterDataStackModules: all four scripts hang off
        // one UNiagaraScriptSource, so walking each script's graph would visit every module once
        // per script.
        TArray<const UNiagaraGraph*> Graphs;
        Graphs.AddUnique(GetGraphFromScript(EmitterData->EmitterSpawnScriptProps.Script));
        Graphs.AddUnique(GetGraphFromScript(EmitterData->EmitterUpdateScriptProps.Script));
        Graphs.AddUnique(GetGraphFromScript(EmitterData->SpawnScriptProps.Script));
        Graphs.AddUnique(GetGraphFromScript(EmitterData->UpdateScriptProps.Script));
        for (const UNiagaraGraph* Graph : Graphs)
        {
            AddGraphOverrideMarks(OutIndex, Graph, UniqueEmitterName, EmitterLabel);
        }
    }

    FRapidIterationOverrideIndex BuildRapidIterationOverrideIndex(const UNiagaraSystem* System)
    {
        FRapidIterationOverrideIndex Index;
        if (!System)
        {
            return Index;
        }

        TArray<const UNiagaraGraph*> SystemGraphs;
        SystemGraphs.AddUnique(GetGraphFromScript(System->GetSystemSpawnScript()));
        SystemGraphs.AddUnique(GetGraphFromScript(System->GetSystemUpdateScript()));
        for (const UNiagaraGraph* Graph : SystemGraphs)
        {
            AddGraphOverrideMarks(Index, Graph, FString(), FString());
        }

        for (const FNiagaraEmitterHandle& Handle : System->GetEmitterHandles())
        {
            const FVersionedNiagaraEmitter Instance = Handle.GetInstance();
            const FString UniqueEmitterName = Instance.Emitter ? Instance.Emitter->GetUniqueEmitterName() : FString();
            AddEmitterDataOverrideMarks(Index, Handle.GetEmitterData(), UniqueEmitterName, Handle.GetName().ToString());
        }
        return Index;
    }

    TSharedPtr<FJsonObject> BuildParametersJson(const UNiagaraSystem* System, const FString& NameFilter)
    {
        const FRapidIterationOverrideIndex OverrideIndex = BuildRapidIterationOverrideIndex(System);

        TSharedPtr<FJsonObject> Obj = MakeObject();
        Obj->SetStringField(TEXT("assetKind"), TEXT("NiagaraSystem"));
        Obj->SetStringField(TEXT("systemPath"), GetObjectPathSafe(System));
        Obj->SetStringField(TEXT("rapidIterationNote"), RapidIterationStoreNote);
        Obj->SetArrayField(TEXT("user"), System ? BuildParameterStoreArray(System->GetExposedParameters(), TEXT("user"), NameFilter) : TArray<TSharedPtr<FJsonValue>>());
        Obj->SetArrayField(TEXT("systemSpawnRapidIteration"), System && System->GetSystemSpawnScript() ? BuildParameterStoreArray(System->GetSystemSpawnScript()->RapidIterationParameters, TEXT("systemSpawnRapidIteration"), NameFilter, &OverrideIndex) : TArray<TSharedPtr<FJsonValue>>());
        Obj->SetArrayField(TEXT("systemUpdateRapidIteration"), System && System->GetSystemUpdateScript() ? BuildParameterStoreArray(System->GetSystemUpdateScript()->RapidIterationParameters, TEXT("systemUpdateRapidIteration"), NameFilter, &OverrideIndex) : TArray<TSharedPtr<FJsonValue>>());

        TArray<TSharedPtr<FJsonValue>> Emitters;
        if (System)
        {
            for (const FNiagaraEmitterHandle& Handle : System->GetEmitterHandles())
            {
                const FVersionedNiagaraEmitterData* EmitterData = Handle.GetEmitterData();
                TSharedPtr<FJsonObject> EmitterObj = MakeObject();
                EmitterObj->SetStringField(TEXT("name"), Handle.GetName().ToString());
                EmitterObj->SetStringField(TEXT("id"), Handle.GetId().ToString());
                EmitterObj->SetArrayField(TEXT("rendererBindings"), EmitterData ? BuildParameterStoreArray(EmitterData->RendererBindings, TEXT("rendererBindings"), NameFilter) : TArray<TSharedPtr<FJsonValue>>());
                EmitterObj->SetArrayField(TEXT("spawnRapidIteration"), EmitterData && EmitterData->SpawnScriptProps.Script ? BuildParameterStoreArray(EmitterData->SpawnScriptProps.Script->RapidIterationParameters, TEXT("spawnRapidIteration"), NameFilter, &OverrideIndex) : TArray<TSharedPtr<FJsonValue>>());
                EmitterObj->SetArrayField(TEXT("updateRapidIteration"), EmitterData && EmitterData->UpdateScriptProps.Script ? BuildParameterStoreArray(EmitterData->UpdateScriptProps.Script->RapidIterationParameters, TEXT("updateRapidIteration"), NameFilter, &OverrideIndex) : TArray<TSharedPtr<FJsonValue>>());
                Emitters.Add(MakeObjectValue(EmitterObj));
            }
        }
        Obj->SetArrayField(TEXT("emitters"), Emitters);
        return Obj;
    }

    TSharedPtr<FJsonObject> BuildEmitterParametersJson(const UNiagaraEmitter* Emitter, const FString& NameFilter)
    {
        TSharedPtr<FJsonObject> Obj = MakeObject();
        Obj->SetStringField(TEXT("assetKind"), TEXT("NiagaraEmitter"));
        Obj->SetStringField(TEXT("emitterPath"), GetObjectPathSafe(Emitter));
        Obj->SetStringField(TEXT("rapidIterationNote"), RapidIterationStoreNote);
        const FVersionedNiagaraEmitterData* EmitterData = Emitter ? Emitter->GetLatestEmitterData() : nullptr;
        FRapidIterationOverrideIndex OverrideIndex;
        AddEmitterDataOverrideMarks(
            OverrideIndex,
            EmitterData,
            Emitter ? Emitter->GetUniqueEmitterName() : FString(),
            Emitter ? Emitter->GetName() : FString());
        Obj->SetArrayField(TEXT("rendererBindings"), EmitterData ? BuildParameterStoreArray(EmitterData->RendererBindings, TEXT("rendererBindings"), NameFilter) : TArray<TSharedPtr<FJsonValue>>());
        Obj->SetArrayField(TEXT("spawnRapidIteration"), EmitterData && EmitterData->SpawnScriptProps.Script ? BuildParameterStoreArray(EmitterData->SpawnScriptProps.Script->RapidIterationParameters, TEXT("spawnRapidIteration"), NameFilter, &OverrideIndex) : TArray<TSharedPtr<FJsonValue>>());
        Obj->SetArrayField(TEXT("updateRapidIteration"), EmitterData && EmitterData->UpdateScriptProps.Script ? BuildParameterStoreArray(EmitterData->UpdateScriptProps.Script->RapidIterationParameters, TEXT("updateRapidIteration"), NameFilter, &OverrideIndex) : TArray<TSharedPtr<FJsonValue>>());
        return Obj;
    }

    TSharedPtr<FJsonObject> BuildStackJson(const UNiagaraSystem* System)
    {
        TSharedPtr<FJsonObject> Obj = MakeObject();
        Obj->SetStringField(TEXT("assetKind"), TEXT("NiagaraSystem"));
        Obj->SetStringField(TEXT("systemPath"), GetObjectPathSafe(System));
        Obj->SetArrayField(TEXT("modules"), BuildSystemStackArray(System));
        return Obj;
    }

    TSharedPtr<FJsonObject> BuildEmitterStackJson(const UNiagaraEmitter* Emitter)
    {
        TSharedPtr<FJsonObject> Obj = MakeObject();
        Obj->SetStringField(TEXT("assetKind"), TEXT("NiagaraEmitter"));
        Obj->SetStringField(TEXT("emitterPath"), GetObjectPathSafe(Emitter));
        Obj->SetArrayField(TEXT("modules"), BuildEmitterStackArray(Emitter));
        return Obj;
    }

    TSharedPtr<FJsonObject> BuildGraphsJson(const UNiagaraSystem* System)
    {
        TSharedPtr<FJsonObject> Obj = MakeObject();
        Obj->SetStringField(TEXT("assetKind"), TEXT("NiagaraSystem"));
        Obj->SetStringField(TEXT("systemPath"), GetObjectPathSafe(System));
        Obj->SetArrayField(TEXT("graphs"), BuildSystemGraphArray(System));
        return Obj;
    }

    TSharedPtr<FJsonObject> BuildEmitterGraphsJson(const UNiagaraEmitter* Emitter)
    {
        TSharedPtr<FJsonObject> Obj = MakeObject();
        Obj->SetStringField(TEXT("assetKind"), TEXT("NiagaraEmitter"));
        Obj->SetStringField(TEXT("emitterPath"), GetObjectPathSafe(Emitter));
        Obj->SetArrayField(TEXT("graphs"), BuildEmitterGraphArray(Emitter));
        return Obj;
    }

    TSharedPtr<FJsonObject> BuildScriptGraphsJson(const UNiagaraScript* Script)
    {
        TSharedPtr<FJsonObject> Obj = MakeObject();
        Obj->SetStringField(TEXT("assetKind"), TEXT("NiagaraScript"));
        Obj->SetStringField(TEXT("scriptPath"), GetObjectPathSafe(Script));
        Obj->SetStringField(TEXT("scriptName"), Script ? Script->GetName() : FString());
        Obj->SetStringField(TEXT("scriptUsage"), Script ? EnumToString(Script->GetUsage()) : FString());
        Obj->SetArrayField(TEXT("graphs"), BuildScriptGraphArray(Script));
        return Obj;
    }

    TSharedPtr<FJsonObject> BuildCompileJson(const UNiagaraSystem* System)
    {
        TSharedPtr<FJsonObject> Obj = MakeObject();
        Obj->SetStringField(TEXT("assetKind"), TEXT("NiagaraSystem"));
        Obj->SetStringField(TEXT("systemPath"), GetObjectPathSafe(System));
        Obj->SetArrayField(TEXT("scripts"), MakeAuthoredScriptArray(BuildSystemCompileScriptArray(System)));
        Obj->SetArrayField(TEXT("issues"), BuildSystemAuthoredIssues(System));
        return Obj;
    }

    TSharedPtr<FJsonObject> BuildEmitterCompileJson(const UNiagaraEmitter* Emitter)
    {
        TSharedPtr<FJsonObject> Obj = MakeObject();
        Obj->SetStringField(TEXT("assetKind"), TEXT("NiagaraEmitter"));
        Obj->SetStringField(TEXT("emitterPath"), GetObjectPathSafe(Emitter));
        Obj->SetArrayField(TEXT("scripts"), MakeAuthoredScriptArray(BuildEmitterCompileScriptArray(Emitter)));
        Obj->SetArrayField(TEXT("issues"), BuildEmitterAuthoredIssues(Emitter));
        return Obj;
    }

    TSharedPtr<FJsonObject> BuildScriptCompileJson(const UNiagaraScript* Script)
    {
        TSharedPtr<FJsonObject> Obj = MakeObject();
        Obj->SetStringField(TEXT("assetKind"), TEXT("NiagaraScript"));
        Obj->SetStringField(TEXT("scriptPath"), GetObjectPathSafe(Script));
        Obj->SetStringField(TEXT("scriptName"), Script ? Script->GetName() : FString());
        Obj->SetStringField(TEXT("scriptUsage"), Script ? EnumToString(Script->GetUsage()) : FString());
        Obj->SetArrayField(TEXT("scripts"), MakeAuthoredScriptArray(BuildScriptCompileScriptArray(Script)));
        Obj->SetArrayField(TEXT("issues"), BuildScriptAuthoredIssues(Script));
        return Obj;
    }

    TSharedPtr<FJsonObject> BuildCompileDiagnosticsJson(const UNiagaraSystem* System)
    {
        TSharedPtr<FJsonObject> Obj = MakeObject();
        Obj->SetStringField(TEXT("assetKind"), TEXT("NiagaraSystem"));
        Obj->SetStringField(TEXT("systemPath"), GetObjectPathSafe(System));
        Obj->SetBoolField(TEXT("valid"), System ? System->IsValid() : false);
        Obj->SetBoolField(TEXT("hasOutstandingCompilationRequests"), System ? System->HasOutstandingCompilationRequests(true) : false);
        // HasActiveCompilations() was introduced in UE 5.6; UE 5.4 and 5.5 only have HasOutstandingCompilationRequests().
        // In 5.6 HasActiveCompilations() checks only ActiveCompilations.IsEmpty(), while
        // HasOutstandingCompilationRequests() additionally covers NeedsRequestCompile() and
        // optional GPU-shader stages. The 5.4 fallback uses HasOutstandingCompilationRequests()
        // (no GPU arg), which is a slight superset but the closest available equivalent.
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 6, 0)
        Obj->SetBoolField(TEXT("hasActiveCompilations"), System ? System->HasActiveCompilations() : false);
#else
        Obj->SetBoolField(TEXT("hasActiveCompilations"), System ? System->HasOutstandingCompilationRequests() : false);
#endif
        const TArray<TSharedPtr<FJsonValue>> Scripts = BuildSystemCompileScriptArray(System);
        Obj->SetArrayField(TEXT("scripts"), Scripts);

        // UE 5.6 ships fx.Niagara.OnDemandCompile defaulted on, deferring Niagara compile until
        // the editor opens the system or an FX component spawns it. Surface that as a diagnostic
        // so consumers can distinguish post-load deferred state from a genuinely broken system.
        const bool bCompileDeferredOnLoad = NiagaraDecompileHelpers::IsNiagaraOnDemandCompileEnabled();
        Obj->SetBoolField(TEXT("compileDeferredOnLoad"), bCompileDeferredOnLoad);

        TArray<TSharedPtr<FJsonValue>> Issues;
        if (bCompileDeferredOnLoad)
        {
            TSharedPtr<FJsonObject> Issue = MakeObject();
            Issue->SetStringField(TEXT("severity"), TEXT("info"));
            Issue->SetStringField(TEXT("code"), TEXT("COMPILE_DEFERRED_ON_LOAD"));
            Issue->SetStringField(TEXT("message"), TEXT("UE 5.6 fx.Niagara.OnDemandCompile is enabled; compile state shown reflects post-load deferred state, not a fresh compile."));
            Issues.Add(MakeObjectValue(Issue));
        }
        Issues.Append(BuildSystemAuthoredIssues(System));

        // readyToRun and the COMPILE_STATE_UNINITIALIZED issue are written last so we can null
        // out readyToRun when scripts are still in NCS_Unknown — the boolean would otherwise lie.
        const bool bUninitializedCompileState = ScriptsHaveUninitializedCompileStatus(Scripts);
        if (bUninitializedCompileState)
        {
            Obj->SetField(TEXT("readyToRun"), MakeShared<FJsonValueNull>());
            TSharedPtr<FJsonObject> Issue = MakeObject();
            Issue->SetStringField(TEXT("severity"), TEXT("info"));
            Issue->SetStringField(TEXT("code"), TEXT("COMPILE_STATE_UNINITIALIZED"));
            Issue->SetStringField(TEXT("message"), TEXT("One or more scripts report no compile state; this dump reflects post-load state, not post-compile state. Open the asset in the editor or spawn it as an FX component to populate compile state, then re-dump."));
            Issues.Add(MakeObjectValue(Issue));
        }
        else
        {
            Obj->SetBoolField(TEXT("readyToRun"), IsSystemReadyToRunWithoutLoading(System));
        }
        Obj->SetArrayField(TEXT("issues"), Issues);
        return Obj;
    }

    TSharedPtr<FJsonObject> BuildEmitterCompileDiagnosticsJson(const UNiagaraEmitter* Emitter)
    {
        TSharedPtr<FJsonObject> Obj = MakeObject();
        const FVersionedNiagaraEmitterData* EmitterData = Emitter ? Emitter->GetLatestEmitterData() : nullptr;
        Obj->SetStringField(TEXT("assetKind"), TEXT("NiagaraEmitter"));
        Obj->SetStringField(TEXT("emitterPath"), GetObjectPathSafe(Emitter));
        Obj->SetBoolField(TEXT("valid"), EmitterData ? EmitterData->IsValid() : false);
        const TArray<TSharedPtr<FJsonValue>> Scripts = BuildEmitterCompileScriptArray(Emitter);
        Obj->SetArrayField(TEXT("scripts"), Scripts);

        // Mirror the System path: surface fx.Niagara.OnDemandCompile so the standalone-emitter
        // schema matches the system schema and consumers see the same deferred-on-load signal.
        const bool bCompileDeferredOnLoad = NiagaraDecompileHelpers::IsNiagaraOnDemandCompileEnabled();
        Obj->SetBoolField(TEXT("compileDeferredOnLoad"), bCompileDeferredOnLoad);

        TArray<TSharedPtr<FJsonValue>> Issues;
        if (bCompileDeferredOnLoad)
        {
            TSharedPtr<FJsonObject> Issue = MakeObject();
            Issue->SetStringField(TEXT("severity"), TEXT("info"));
            Issue->SetStringField(TEXT("code"), TEXT("COMPILE_DEFERRED_ON_LOAD"));
            Issue->SetStringField(TEXT("message"), TEXT("UE 5.6 fx.Niagara.OnDemandCompile is enabled; compile state shown reflects post-load deferred state, not a fresh compile."));
            Issues.Add(MakeObjectValue(Issue));
        }
        Issues.Append(BuildEmitterAuthoredIssues(Emitter));

        // readyToRun and the COMPILE_STATE_UNINITIALIZED issue are written last so we can null
        // out readyToRun when scripts are still in NCS_Unknown — the boolean would otherwise lie.
        const bool bUninitializedCompileState = ScriptsHaveUninitializedCompileStatus(Scripts);
        if (bUninitializedCompileState)
        {
            Obj->SetField(TEXT("readyToRun"), MakeShared<FJsonValueNull>());
            TSharedPtr<FJsonObject> Issue = MakeObject();
            Issue->SetStringField(TEXT("severity"), TEXT("info"));
            Issue->SetStringField(TEXT("code"), TEXT("COMPILE_STATE_UNINITIALIZED"));
            Issue->SetStringField(TEXT("message"), TEXT("One or more scripts report no compile state; this dump reflects post-load state, not post-compile state. Open the asset in the editor or spawn it as an FX component to populate compile state, then re-dump."));
            Issues.Add(MakeObjectValue(Issue));
        }
        else
        {
            Obj->SetBoolField(TEXT("readyToRun"), EmitterData ? EmitterData->IsReadyToRun() : false);
        }
        Obj->SetArrayField(TEXT("issues"), Issues);
        return Obj;
    }

    TSharedPtr<FJsonObject> BuildScriptCompileDiagnosticsJson(const UNiagaraScript* Script)
    {
        TSharedPtr<FJsonObject> Obj = MakeObject();
        Obj->SetStringField(TEXT("assetKind"), TEXT("NiagaraScript"));
        Obj->SetStringField(TEXT("scriptPath"), GetObjectPathSafe(Script));
        Obj->SetStringField(TEXT("scriptName"), Script ? Script->GetName() : FString());
        Obj->SetStringField(TEXT("scriptUsage"), Script ? EnumToString(Script->GetUsage()) : FString());
        Obj->SetBoolField(TEXT("valid"), Script ? Script->IsCompilable() : false);
        const TArray<TSharedPtr<FJsonValue>> Scripts = BuildScriptCompileScriptArray(Script);
        Obj->SetArrayField(TEXT("scripts"), Scripts);

        const bool bCompileDeferredOnLoad = NiagaraDecompileHelpers::IsNiagaraOnDemandCompileEnabled();
        Obj->SetBoolField(TEXT("compileDeferredOnLoad"), bCompileDeferredOnLoad);

        TArray<TSharedPtr<FJsonValue>> Issues;
        if (bCompileDeferredOnLoad)
        {
            TSharedPtr<FJsonObject> Issue = MakeObject();
            Issue->SetStringField(TEXT("severity"), TEXT("info"));
            Issue->SetStringField(TEXT("code"), TEXT("COMPILE_DEFERRED_ON_LOAD"));
            Issue->SetStringField(TEXT("message"), TEXT("UE 5.6 fx.Niagara.OnDemandCompile is enabled; compile state shown reflects post-load deferred state, not a fresh compile."));
            Issues.Add(MakeObjectValue(Issue));
        }
        Issues.Append(BuildScriptAuthoredIssues(Script));

        const bool bUninitializedCompileState = ScriptsHaveUninitializedCompileStatus(Scripts);
        if (bUninitializedCompileState)
        {
            Obj->SetField(TEXT("readyToRun"), MakeShared<FJsonValueNull>());
            TSharedPtr<FJsonObject> Issue = MakeObject();
            Issue->SetStringField(TEXT("severity"), TEXT("info"));
            Issue->SetStringField(TEXT("code"), TEXT("COMPILE_STATE_UNINITIALIZED"));
            Issue->SetStringField(TEXT("message"), TEXT("One or more scripts report no compile state; this dump reflects post-load state, not post-compile state. Open the asset in the editor or spawn it as an FX component to populate compile state, then re-dump."));
            Issues.Add(MakeObjectValue(Issue));
        }
        else
        {
            Obj->SetBoolField(TEXT("readyToRun"), Script ? Script->IsReadyToRun(ENiagaraSimTarget::CPUSim) : false);
        }
        Obj->SetArrayField(TEXT("issues"), Issues);
        return Obj;
    }
}

namespace NiagaraDecompileHelpers
{
    bool IsNiagaraOnDemandCompileEnabled()
    {
        const IConsoleVariable* CV = IConsoleManager::Get().FindConsoleVariable(TEXT("fx.Niagara.OnDemandCompile"));
        return CV && CV->GetBool();
    }

    TArray<UNiagaraNodeFunctionCall*> CollectAndSortFunctionCalls(const UNiagaraGraph* Graph)
    {
        TArray<UNiagaraNodeFunctionCall*> Result;
        if (!Graph)
        {
            return Result;
        }
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (UNiagaraNodeFunctionCall* FunctionCall = Cast<UNiagaraNodeFunctionCall>(Node))
            {
                Result.Add(FunctionCall);
            }
        }
        Result.Sort([](const UNiagaraNodeFunctionCall& A, const UNiagaraNodeFunctionCall& B)
        {
            if (A.NodePosY != B.NodePosY)
            {
                return A.NodePosY < B.NodePosY;
            }
            return A.NodeGuid.ToString() < B.NodeGuid.ToString();
        });
        return Result;
    }

    bool IsGpuIncompatibleFunctionCall(const UNiagaraNodeFunctionCall* Node)
    {
        return Node && !Node->Signature.Name.IsNone() && !Node->Signature.bSupportsGPU;
    }

    namespace
    {
        void AppendGpuIncompatibleModulesFromScript(
            TArray<FNiagaraGpuIncompatibleModule>& Result,
            const FString& StackName,
            const UNiagaraScript* Script)
        {
            for (const UNiagaraNodeFunctionCall* Node : CollectAndSortFunctionCalls(NiagaraJsonHelpers::GetGraphFromScript(Script)))
            {
                if (!IsGpuIncompatibleFunctionCall(Node))
                {
                    continue;
                }
                FNiagaraGpuIncompatibleModule Entry;
                Entry.ModuleName = Node->GetFunctionName();
                Entry.NodeName = Node->GetName();
                Entry.StackName = StackName;
                Result.Add(MoveTemp(Entry));
            }
        }
    }

    TArray<FNiagaraGpuIncompatibleModule> CollectGpuIncompatibleModules(const FVersionedNiagaraEmitterData* EmitterData)
    {
        TArray<FNiagaraGpuIncompatibleModule> Result;
        if (!EmitterData || EmitterData->SimTarget != ENiagaraSimTarget::GPUComputeSim)
        {
            return Result;
        }

        AppendGpuIncompatibleModulesFromScript(Result, TEXT("EmitterSpawn"), EmitterData->EmitterSpawnScriptProps.Script);
        AppendGpuIncompatibleModulesFromScript(Result, TEXT("EmitterUpdate"), EmitterData->EmitterUpdateScriptProps.Script);
        AppendGpuIncompatibleModulesFromScript(Result, TEXT("ParticleSpawn"), EmitterData->SpawnScriptProps.Script);
        AppendGpuIncompatibleModulesFromScript(Result, TEXT("ParticleUpdate"), EmitterData->UpdateScriptProps.Script);
        for (const FNiagaraEventScriptProperties& Handler : EmitterData->GetEventHandlers())
        {
            AppendGpuIncompatibleModulesFromScript(
                Result,
                FString::Printf(TEXT("ParticleEvent:%s"), *Handler.SourceEventName.ToString()),
                Handler.Script);
        }
        const TArray<UNiagaraSimulationStageBase*>& Stages = EmitterData->GetSimulationStages();
        for (const UNiagaraSimulationStageBase* Stage : Stages)
        {
            AppendGpuIncompatibleModulesFromScript(
                Result,
                FString::Printf(TEXT("ParticleSimulationStage:%s"), Stage ? *Stage->GetName() : TEXT("None")),
                Stage ? Stage->Script : nullptr);
        }
        AppendGpuIncompatibleModulesFromScript(Result, TEXT("ParticleGPUCompute"), EmitterData->GetGPUComputeScript());
        return Result;
    }
}
