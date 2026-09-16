// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "NIR/NIRDecompiler.h"

#include "Compat/EngineVersionCompat.h"
#include "Compat/JsonKeyCompat.h"
#include "IrCore/IrTextUtils.h"
#include "NIR/NIRTextEmitter.h"

#include "NiagaraCommon.h"
#include "NiagaraConstants.h"
#include "NiagaraEmitter.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraGraph.h"
#include "NiagaraNodeFunctionCall.h"
#include "NiagaraNodeParameterMapSet.h"
#include "NiagaraParameterStore.h"
#include "NiagaraRendererProperties.h"
#include "NiagaraScript.h"
#include "NiagaraScriptVariable.h"
#include "NiagaraSimulationStageBase.h"
#include "NiagaraSystem.h"
#include "NiagaraTypes.h"

#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "UObject/UnrealType.h"

#include "ViewModels/Stack/NiagaraParameterHandle.h"

#include "Handlers/Niagara/NiagaraDecompileHelpers.h"
#include "Handlers/Niagara/NiagaraDumpBuilder.h"
#include "Handlers/Niagara/NiagaraJsonHelpers.h"
#include "Handlers/Niagara/NiagaraModelBuilder.h"
#include "Handlers/Niagara/NiagaraResetModuleInputHelpers.h"

namespace
{
    FString PathQuoted(const UObject* Object)
    {
        return Object ? NIRTextEmitter::Quote(Object->GetPathName()) : FString(TEXT("\"\""));
    }

    FString ShortClassName(const UClass* Class)
    {
        if (!Class)
        {
            return FString(TEXT("None"));
        }
        return NIRTextEmitter::FormatNameToken(Class->GetName());
    }

    const TCHAR* BoolLiteral(bool bValue)
    {
        return bValue ? TEXT("true") : TEXT("false");
    }

    FString JsonValueToNirLiteral(const TSharedPtr<FJsonValue>& Value);

    FString JsonArrayToNirLiteral(const TArray<TSharedPtr<FJsonValue>>& Values)
    {
        TArray<FString> Parts;
        for (const TSharedPtr<FJsonValue>& Value : Values)
        {
            Parts.Add(JsonValueToNirLiteral(Value));
        }
        return FString::Printf(TEXT("[%s]"), *FString::Join(Parts, TEXT(", ")));
    }

    FString JsonObjectToNirLiteral(const TSharedPtr<FJsonObject>& Object)
    {
        if (!Object.IsValid())
        {
            return TEXT("{}");
        }
        TArray<FString> Keys;
        for (const auto& Pair : Object->Values)
        {
            Keys.Add(EARGCompat::JsonKeyToString(Pair.Key));
        }
        Keys.Sort();

        TArray<FString> Parts;
        for (const FString& Key : Keys)
        {
            Parts.Add(FString::Printf(
                TEXT("%s: %s"),
                *NIRTextEmitter::FormatNameToken(Key),
                *JsonValueToNirLiteral(Object->TryGetField(Key))));
        }
        return FString::Printf(TEXT("{%s}"), *FString::Join(Parts, TEXT(", ")));
    }

    FString JsonValueToNirLiteral(const TSharedPtr<FJsonValue>& Value)
    {
        if (!Value.IsValid() || Value->Type == EJson::Null)
        {
            return TEXT("null");
        }
        switch (Value->Type)
        {
        case EJson::String:
            return NIRTextEmitter::Quote(Value->AsString());
        case EJson::Number:
            return FString::SanitizeFloat(Value->AsNumber());
        case EJson::Boolean:
            return BoolLiteral(Value->AsBool());
        case EJson::Array:
            return JsonArrayToNirLiteral(Value->AsArray());
        case EJson::Object:
            return JsonObjectToNirLiteral(Value->AsObject());
        default:
            return TEXT("null");
        }
    }

    FString GetModeledTypeName(const TSharedPtr<FJsonObject>& Parameter)
    {
        if (!Parameter.IsValid())
        {
            return TEXT("Unknown");
        }
        const TSharedPtr<FJsonValue> TypeField = Parameter->TryGetField(TEXT("type"));
        const TSharedPtr<FJsonObject> TypeObject = TypeField.IsValid() && TypeField->Type == EJson::Object
            ? TypeField->AsObject()
            : nullptr;
        FString TypeName;
        if (TypeObject.IsValid())
        {
            TypeObject->TryGetStringField(TEXT("name"), TypeName);
        }
        return TypeName.IsEmpty() ? TEXT("Unknown") : TypeName;
    }

    void AppendModeledParameters(
        FNIRTextEmitter& Emitter,
        const TArray<TSharedPtr<FJsonValue>>& Parameters,
        const FString& Prefix,
        const FString& Scope)
    {
        for (const TSharedPtr<FJsonValue>& Value : Parameters)
        {
            const TSharedPtr<FJsonObject> Parameter = Value.IsValid() && Value->Type == EJson::Object
                ? Value->AsObject()
                : nullptr;
            if (!Parameter.IsValid())
            {
                continue;
            }
            FString Name;
            Parameter->TryGetStringField(TEXT("name"), Name);
            if (Name.IsEmpty())
            {
                continue;
            }
            const FString TypeName = GetModeledTypeName(Parameter);
            const TSharedPtr<FJsonValue> ParameterValue = Parameter->TryGetField(TEXT("value"));
            Emitter.AppendLine(FString::Printf(
                TEXT("%s %s : %s = %s @scope %s"),
                *Prefix,
                *NIRTextEmitter::FormatNameToken(Name),
                *NIRTextEmitter::FormatNameToken(TypeName),
                *JsonValueToNirLiteral(ParameterValue),
                *NIRTextEmitter::FormatNameToken(Scope)));
        }
    }

    void AppendParameterStoreValues(
        FNIRTextEmitter& Emitter,
        const FNiagaraParameterStore& Store,
        const FString& Scope,
        const UObject* Owner,
        const FString& Prefix)
    {
        AppendModeledParameters(
            Emitter,
            NiagaraModelBuilder::BuildParameterStoreModel(Store, Scope, Owner),
            Prefix,
            Scope);
    }

    bool SplitRapidIterationName(
        const FNiagaraVariable& Variable,
        ENiagaraScriptUsage Usage,
        FString& OutFunctionCallName,
        FString& OutInputName)
    {
        TArray<FString> Parts;
        Variable.GetName().ToString().ParseIntoArray(Parts, TEXT("."));
        const bool bSystemScript = Usage == ENiagaraScriptUsage::SystemSpawnScript
            || Usage == ENiagaraScriptUsage::SystemUpdateScript;
        const int32 FunctionIndex = bSystemScript ? 1 : 2;
        const int32 InputIndex = FunctionIndex + 1;
        if (Parts.Num() <= InputIndex || Parts[0] != FNiagaraConstants::RapidIterationParametersNamespaceString)
        {
            return false;
        }
        OutFunctionCallName = Parts[FunctionIndex];
        OutInputName = FString::Join(TArrayView<FString>(Parts).Slice(InputIndex, Parts.Num() - InputIndex), TEXT("."));
        return !OutFunctionCallName.IsEmpty() && !OutInputName.IsEmpty();
    }

    bool IsModuleSourceDefault(
        const UNiagaraScript& Script,
        const FNiagaraVariable& Variable,
        const uint8* ValueData)
    {
        if (!ValueData)
        {
            return false;
        }

        FString FunctionCallName;
        FString InputName;
        if (!SplitRapidIterationName(Variable, Script.GetUsage(), FunctionCallName, InputName))
        {
            return false;
        }

        const UNiagaraGraph* StackGraph = NiagaraJsonHelpers::GetGraphFromScript(&Script);
        for (UNiagaraNodeFunctionCall* FunctionCall : NiagaraDecompileHelpers::CollectAndSortFunctionCalls(StackGraph))
        {
            if (!FunctionCall || FunctionCall->GetFunctionName() != FunctionCallName)
            {
                continue;
            }
            const UNiagaraGraph* CalledGraph = FunctionCall->GetCalledGraph();
            if (!CalledGraph)
            {
                continue;
            }

            const FName ModuleInputName(*(FNiagaraConstants::ModuleNamespaceString + TEXT(".") + InputName));
            const FNiagaraVariable ModuleInput(Variable.GetType(), ModuleInputName);
            const TObjectPtr<UNiagaraScriptVariable>* ScriptVariablePtr = CalledGraph->GetAllMetaData().Find(ModuleInput);
            const UNiagaraScriptVariable* ScriptVariable = ScriptVariablePtr ? ScriptVariablePtr->Get() : nullptr;
            const uint8* DefaultData = ScriptVariable && ScriptVariable->DefaultMode == ENiagaraDefaultMode::Value
                ? ScriptVariable->GetDefaultValueData()
                : nullptr;
            if (DefaultData
                && ScriptVariable->Variable.GetSizeInBytes() == Variable.GetSizeInBytes()
                && FMemory::Memcmp(ValueData, DefaultData, Variable.GetSizeInBytes()) == 0)
            {
                return true;
            }
        }
        return false;
    }

    void AppendRapidIterationStore(
        FNIRTextEmitter& Emitter,
        const UNiagaraScript* Script,
        const FString& Scope,
        const UObject* Owner)
    {
        if (!Script)
        {
            return;
        }
        TArray<FNiagaraVariable> Vars;
        Script->RapidIterationParameters.GetParameters(Vars);
        TSet<FName> AuthoredNames;
        for (const FNiagaraVariable& Variable : Vars)
        {
            const uint8* ValueData = Script->RapidIterationParameters.GetParameterData(Variable);
            if (!IsModuleSourceDefault(*Script, Variable, ValueData))
            {
                AuthoredNames.Add(Variable.GetName());
            }
        }
        if (AuthoredNames.IsEmpty())
        {
            return;
        }

        TArray<TSharedPtr<FJsonValue>> Parameters =
            NiagaraModelBuilder::BuildParameterStoreModel(Script->RapidIterationParameters, Scope, Owner);
        Parameters.RemoveAll([&AuthoredNames](const TSharedPtr<FJsonValue>& Value)
        {
            const TSharedPtr<FJsonObject> Parameter = Value.IsValid() && Value->Type == EJson::Object
                ? Value->AsObject()
                : nullptr;
            FString Name;
            return !Parameter.IsValid()
                || !Parameter->TryGetStringField(TEXT("name"), Name)
                || !AuthoredNames.Contains(FName(*Name));
        });
        AppendModeledParameters(Emitter, Parameters, TEXT("rapid"), Scope);
    }

    FString FormatBox(const FBox& Box)
    {
        return FString::Printf(
            TEXT("valid=%s min=(%s, %s, %s) max=(%s, %s, %s)"),
            BoolLiteral(Box.IsValid != 0),
            *FString::SanitizeFloat(Box.Min.X),
            *FString::SanitizeFloat(Box.Min.Y),
            *FString::SanitizeFloat(Box.Min.Z),
            *FString::SanitizeFloat(Box.Max.X),
            *FString::SanitizeFloat(Box.Max.Y),
            *FString::SanitizeFloat(Box.Max.Z));
    }

    constexpr const TCHAR* EnabledSuffix = TEXT(" enabled");
    constexpr const TCHAR* DisabledSuffix = TEXT(" disabled");
    constexpr const TCHAR* NIRUnresolvedPinWarningCode = TEXT("NIR_UNRESOLVED_PIN");
    constexpr const TCHAR* UndefinedStaticSwitchInputName = TEXT("Undefined parameter name");

    bool IsUnresolvedStaticSwitchInputName(const FString& InputName)
    {
        return InputName.IsEmpty()
            || InputName == UndefinedStaticSwitchInputName
            || FName(*InputName).IsNone();
    }

    // Caller-pin literal default for non-static inputs. Returns true if Pin has a
    // non-empty default value usable as a literal.
    bool TryEmitLiteralFromPin(const UEdGraphPin* Pin, FString& OutLiteral)
    {
        if (!Pin || Pin->Direction != EGPD_Input)
        {
            return false;
        }
        if (!Pin->LinkedTo.IsEmpty())
        {
            return false;
        }
        if (Pin->DefaultValue.IsEmpty())
        {
            return false;
        }
        OutLiteral = Pin->DefaultValue;
        return true;
    }

    // Map module-input name (un-aliased, matches the function-call's pin names) →
    // the override-node input pin that drives it. The override-node pin name carries
    // the aliased "<ModuleFunctionName>.<InputName>" handle; this routine strips
    // the alias so per-input lookups against the function-call use un-aliased names.
    TMap<FName, UEdGraphPin*> CollectOverridePinsByInputName(
        const UNiagaraNodeFunctionCall& Node,
        UNiagaraNodeParameterMapSet* OverrideNode)
    {
        TMap<FName, UEdGraphPin*> Result;
        if (!OverrideNode)
        {
            return Result;
        }
        TArray<UEdGraphPin*> OverridePins;
        OverrideNode->GetInputPins(OverridePins);
        const FName ModuleFunctionName(*Node.GetFunctionName());
        for (UEdGraphPin* Pin : OverridePins)
        {
            if (!Pin)
            {
                continue;
            }
            const FNiagaraParameterHandle Handle(Pin->PinName);
            if (Handle.GetNamespace() == ModuleFunctionName)
            {
                Result.Add(Handle.GetName(), Pin);
            }
        }
        return Result;
    }

    // Parses a {name, value} entry from BuildStaticSwitchInputs JSON, appending the
    // formatted "static X = Y" line through the emitter and returning the input name.
    bool EmitStaticSwitchEntry(
        FNIRTextEmitter& Emitter,
        const TSharedPtr<FJsonObject>& Entry,
        TSet<FString>& EmittedStaticSwitchLines,
        FString& OutInputName)
    {
        if (!Entry.IsValid())
        {
            return false;
        }
        Entry->TryGetStringField(TEXT("name"), OutInputName);
        if (IsUnresolvedStaticSwitchInputName(OutInputName))
        {
            Emitter.AppendLine(FString::Printf(
                TEXT("# warning %s static switch input has no resolved parameter name"),
                NIRUnresolvedPinWarningCode));
            Emitter.Warn(FString::Printf(
                TEXT("%s: static switch input has no resolved parameter name."),
                NIRUnresolvedPinWarningCode));
            return true;
        }

        FString Rendered;
        const TSharedPtr<FJsonValue> ValueField = Entry->TryGetField(TEXT("value"));
        if (ValueField.IsValid())
        {
            if (ValueField->Type == EJson::Boolean)
            {
                Rendered = ValueField->AsBool() ? TEXT("true") : TEXT("false");
            }
            else if (ValueField->Type == EJson::Number)
            {
                Rendered = FString::SanitizeFloat(ValueField->AsNumber());
            }
            else if (ValueField->Type == EJson::String)
            {
                Rendered = ValueField->AsString();
            }
        }
        FString Source;
        Entry->TryGetStringField(TEXT("source"), Source);
        FString MetadataSuffix;
        if (!Source.IsEmpty())
        {
            MetadataSuffix += FString::Printf(TEXT(" @source %s"), *NIRTextEmitter::FormatNameToken(Source));
        }
        const TSharedPtr<FJsonValue> DefaultField = Entry->TryGetField(TEXT("defaultValue"));
        if (DefaultField.IsValid())
        {
            MetadataSuffix += FString::Printf(TEXT(" @default %s"), *JsonValueToNirLiteral(DefaultField));
        }
        const FString Line = FString::Printf(
            TEXT("static %s = %s%s"),
            *NIRTextEmitter::FormatNameToken(OutInputName),
            *Rendered,
            *MetadataSuffix);
        if (!EmittedStaticSwitchLines.Contains(Line))
        {
            EmittedStaticSwitchLines.Add(Line);
            Emitter.AppendLine(Line);
        }
        return true;
    }

    void AppendOverrideInputs(
        FNIRTextEmitter& Emitter,
        UNiagaraNodeFunctionCall* Node,
        const TSet<FName>& StaticSwitchInputNames,
        const TMap<FName, UEdGraphPin*>& OverridePinsByInputName)
    {
        if (!Node)
        {
            return;
        }

        TArray<UEdGraphPin*> InputPins;
        for (UEdGraphPin* Pin : Node->Pins)
        {
            if (!Pin || Pin->Direction != EGPD_Input)
            {
                continue;
            }
            if (StaticSwitchInputNames.Contains(Pin->PinName))
            {
                continue;
            }
            InputPins.Add(Pin);
        }
        InputPins.Sort([](const UEdGraphPin& A, const UEdGraphPin& B)
        {
            return A.PinName.LexicalLess(B.PinName);
        });

        for (UEdGraphPin* Pin : InputPins)
        {
            UEdGraphPin* const* OverridePinPtr = OverridePinsByInputName.Find(Pin->PinName);
            if (OverridePinPtr && *OverridePinPtr)
            {
                // Resolve the full override chain (literal / linked-param /
                // dynamic-input recursion) via the shared expression emitter.
                const FString Expr = EmitInputValueExpr(*OverridePinPtr, Emitter, 0);
                if (!Expr.IsEmpty())
                {
                    Emitter.AppendLine(FString::Printf(
                        TEXT("input %s = %s"),
                        *NIRTextEmitter::FormatNameToken(Pin->PinName.ToString()),
                        *Expr));
                }
                continue;
            }
            FString Literal;
            if (TryEmitLiteralFromPin(Pin, Literal))
            {
                Emitter.AppendLine(FString::Printf(
                    TEXT("input %s = %s"),
                    *NIRTextEmitter::FormatNameToken(Pin->PinName.ToString()),
                    *Literal));
            }
        }
    }

    void AppendModuleRow(FNIRTextEmitter& Emitter, int32 Order, UNiagaraNodeFunctionCall* Node, ENiagaraSimTarget SimTarget)
    {
        if (!Node)
        {
            return;
        }
        const FString ShortName = NIRTextEmitter::FormatNameToken(Node->GetFunctionName());
        const TCHAR* Enabled = Node->IsNodeEnabled() ? EnabledSuffix : DisabledSuffix;
        const FString VersionSuffix = NIRTextEmitter::FormatVersionSuffix(Node->SelectedScriptVersion, Node->FunctionScript);
        const TCHAR* GpuAnnotation =
            SimTarget == ENiagaraSimTarget::GPUComputeSim && NiagaraDecompileHelpers::IsGpuIncompatibleFunctionCall(Node)
                ? TEXT(" # GPU_INCOMPATIBLE_MODULE")
                : TEXT("");
        Emitter.AppendLine(FString::Printf(
            TEXT("module %s%s @%d%s%s"),
            *ShortName,
            *VersionSuffix,
            Order,
            Enabled,
            GpuAnnotation));

        // Compute the static-switch payload once: its name list seeds the AlreadyStatic
        // set the override-input pass needs, and re-running BuildStaticSwitchInputs would
        // double-walk the called graph.
        const TArray<TSharedPtr<FJsonValue>> StaticSwitchInputs =
            NiagaraDumpBuilder::BuildStaticSwitchInputs(Node);
        TSet<FName> StaticSwitchInputNames;
        TSet<FString> EmittedStaticSwitchLines;
        Emitter.SetIndentDepth(Emitter.GetIndentDepth() + 1);
        for (const TSharedPtr<FJsonValue>& Value : StaticSwitchInputs)
        {
            FString InputName;
            if (EmitStaticSwitchEntry(
                Emitter,
                Value.IsValid() ? Value->AsObject() : nullptr,
                EmittedStaticSwitchLines,
                InputName))
            {
                StaticSwitchInputNames.Add(FName(*InputName));
            }
        }

        UNiagaraNodeParameterMapSet* OverrideNode = NiagaraResetModuleInput::FindStackFunctionOverrideNode(*Node);
        const TMap<FName, UEdGraphPin*> OverridePinsByInputName = CollectOverridePinsByInputName(*Node, OverrideNode);
        AppendOverrideInputs(Emitter, Node, StaticSwitchInputNames, OverridePinsByInputName);
        Emitter.SetIndentDepth(Emitter.GetIndentDepth() - 1);
    }

    void AppendStack(FNIRTextEmitter& Emitter, const TCHAR* StackName, const UNiagaraScript* Script, ENiagaraSimTarget SimTarget)
    {
        const UNiagaraGraph* Graph = NiagaraJsonHelpers::GetGraphFromScript(Script);
        TArray<UNiagaraNodeFunctionCall*> Nodes = NiagaraDecompileHelpers::CollectAndSortFunctionCalls(Graph);
        if (Nodes.IsEmpty())
        {
            // Omit empty stacks to keep the text compact. v1a doesn't promise round-tripping.
            return;
        }
        Emitter.EnterScope(FString::Printf(TEXT("stack %s "), StackName));
        for (int32 Index = 0; Index < Nodes.Num(); ++Index)
        {
            AppendModuleRow(Emitter, Index, Nodes[Index], SimTarget);
        }
        Emitter.ExitScope();
    }

    TArray<FString> BuildReflectedFields(UObject* Instance)
    {
        TArray<FString> Fields;
        if (!Instance)
        {
            return Fields;
        }
        UObject* DefaultObject = Instance->GetClass()->GetDefaultObject();
        FReflectedFieldEmitOptions Options;
        Options.FieldSeparator = TEXT(": ");
        Options.bEmitArraysAsBracketList = true;
        // Zero-default rule: a property missing from a renderer / simStage block always
        // means the type's zero value, never "at a class default the reader must look
        // up". Non-zero defaults print with a ` @default` marker instead of vanishing —
        // bSubImageBlend defaults to true, so omitting it read as false and inverted.
        Options.bEmitNonZeroDefaults = true;
        FIrTextUtils::AppendReflectedFields(
            Instance->GetClass(),
            Instance,
            DefaultObject,
            Instance,
            TSet<FName>(),
            [](const FStructProperty*) { return false; },
            [](FName) { return false; },
            Options,
            Fields);
        return Fields;
    }

    void AppendReflectedBlock(FNIRTextEmitter& Emitter, const FString& Header, UObject* Instance)
    {
        const TArray<FString> Fields = BuildReflectedFields(Instance);
        if (Fields.IsEmpty())
        {
            Emitter.AppendLine(Header);
            return;
        }
        Emitter.EnterScope(Header + TEXT(" "));
        for (const FString& Field : Fields)
        {
            Emitter.AppendLine(Field);
        }
        Emitter.ExitScope();
    }

    void AppendBoxLine(FNIRTextEmitter& Emitter, const TCHAR* Name, const FBox& Box)
    {
        Emitter.AppendLine(FString::Printf(TEXT("%s: %s"), Name, *FormatBox(Box)));
    }

    void AppendSystemFlags(FNIRTextEmitter& Emitter, UNiagaraSystem& System)
    {
        Emitter.EnterScope(TEXT("systemFlags "));
        Emitter.AppendLine(FString::Printf(TEXT("needsWarmup: %s"), BoolLiteral(System.NeedsWarmup())));
        Emitter.AppendLine(FString::Printf(TEXT("warmupTime: %s"), *FString::SanitizeFloat(System.GetWarmupTime())));
        Emitter.AppendLine(FString::Printf(TEXT("warmupTickCount: %d"), System.GetWarmupTickCount()));
        Emitter.AppendLine(FString::Printf(TEXT("warmupTickDelta: %s"), *FString::SanitizeFloat(System.GetWarmupTickDelta())));
        Emitter.AppendLine(FString::Printf(TEXT("fixedTickDelta: %s"), BoolLiteral(System.HasFixedTickDelta())));
        Emitter.AppendLine(FString::Printf(TEXT("fixedTickDeltaTime: %s"), *FString::SanitizeFloat(System.GetFixedTickDeltaTime())));
        Emitter.AppendLine(FString::Printf(TEXT("determinism: %s"), BoolLiteral(System.NeedsDeterminism())));
        Emitter.AppendLine(FString::Printf(TEXT("randomSeed: %d"), System.GetRandomSeed()));
        Emitter.AppendLine(FString::Printf(TEXT("fixedBoundsEnabled: %s"), BoolLiteral(System.bFixedBounds != 0)));
        AppendBoxLine(Emitter, TEXT("fixedBounds"), System.GetFixedBounds());
        Emitter.EnterScope(TEXT("scalability "));
        Emitter.AppendLine(FString::Printf(
            TEXT("allowForLocalPlayerFX: %s"),
            BoolLiteral(System.AllowScalabilityForLocalPlayerFX())));
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 4, 0)
        // UNiagaraSystem::GetScalabilityPlatformSet() was added in UE 5.4; 5.3 has no
        // single system-level platform set accessor, so the mask line is emitted as 0.
        Emitter.AppendLine(FString::Printf(
            TEXT("platformsQualityLevelMask: %d"),
            System.GetScalabilityPlatformSet().QualityLevelMask));
#else
        Emitter.AppendLine(FString::Printf(TEXT("platformsQualityLevelMask: %d"), 0));
#endif
        Emitter.AppendLine(FString::Printf(
            TEXT("systemScalabilityOverrides: %d"),
            const_cast<UNiagaraSystem&>(System).GetScalabilityOverrides().Overrides.Num()));
        Emitter.ExitScope();
        Emitter.ExitScope();
    }

    void AppendEmitterFlags(FNIRTextEmitter& Emitter, const FVersionedNiagaraEmitterData& EmitterData)
    {
        Emitter.EnterScope(TEXT("emitterFlags "));
        Emitter.AppendLine(FString::Printf(TEXT("localSpace: %s"), BoolLiteral(EmitterData.bLocalSpace)));
        Emitter.AppendLine(FString::Printf(TEXT("determinism: %s"), BoolLiteral(EmitterData.bDeterminism)));
        Emitter.AppendLine(FString::Printf(TEXT("randomSeed: %d"), EmitterData.RandomSeed));
        Emitter.AppendLine(FString::Printf(
            TEXT("calculateBoundsMode: %s"),
            *NIRTextEmitter::FormatNameToken(StaticEnum<ENiagaraEmitterCalculateBoundMode>()->GetNameStringByValue((int64)EmitterData.CalculateBoundsMode))));
        AppendBoxLine(Emitter, TEXT("fixedBounds"), EmitterData.FixedBounds);
        Emitter.AppendLine(FString::Printf(TEXT("requiresPersistentIds: %s"), BoolLiteral(EmitterData.RequiresPersistentIDs())));
        Emitter.AppendLine(FString::Printf(TEXT("maxGpuParticlesSpawnPerFrame: %d"), EmitterData.MaxGPUParticlesSpawnPerFrame));
        Emitter.AppendLine(FString::Printf(
            TEXT("allocationMode: %s"),
            *NIRTextEmitter::FormatNameToken(StaticEnum<EParticleAllocationMode>()->GetNameStringByValue((int64)EmitterData.AllocationMode))));
        Emitter.AppendLine(FString::Printf(TEXT("preAllocationCount: %d"), EmitterData.PreAllocationCount));
        Emitter.EnterScope(TEXT("scalability "));
        Emitter.AppendLine(FString::Printf(TEXT("platformsQualityLevelMask: %d"), EmitterData.Platforms.QualityLevelMask));
        Emitter.AppendLine(FString::Printf(TEXT("emitterScalabilityOverrides: %d"), EmitterData.ScalabilityOverrides.Overrides.Num()));
        Emitter.ExitScope();
        Emitter.ExitScope();
    }

    void AppendEmitterHandleState(FNIRTextEmitter& Emitter, const FNiagaraEmitterHandle& Handle)
    {
        Emitter.EnterScope(TEXT("handle "));
        Emitter.AppendLine(FString::Printf(TEXT("id: %s"), *Handle.GetId().ToString()));
        Emitter.AppendLine(FString::Printf(TEXT("idName: %s"), *NIRTextEmitter::FormatNameToken(Handle.GetIdName().ToString())));
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 4, 0)
        // ENiagaraEmitterMode / FNiagaraEmitterHandle::GetEmitterMode() (emitter inheritance
        // modes) were added in UE 5.4; 5.3 has no per-handle mode, so the line is omitted.
        Emitter.AppendLine(FString::Printf(
            TEXT("mode: %s"),
            *NIRTextEmitter::FormatNameToken(StaticEnum<ENiagaraEmitterMode>()->GetNameStringByValue((int64)Handle.GetEmitterMode()))));
#endif
        Emitter.AppendLine(FString::Printf(TEXT("uniqueInstanceName: %s"), *NIRTextEmitter::FormatNameToken(Handle.GetUniqueInstanceName())));
        Emitter.ExitScope();
    }

    void AppendRenderers(FNIRTextEmitter& Emitter, const FVersionedNiagaraEmitterData* EmitterData)
    {
        if (!EmitterData)
        {
            return;
        }
        const TArray<UNiagaraRendererProperties*>& Renderers = EmitterData->GetRenderers();
        for (int32 Index = 0; Index < Renderers.Num(); ++Index)
        {
            UNiagaraRendererProperties* Renderer = Renderers[Index];
            if (!Renderer)
            {
                continue;
            }
            const TCHAR* Enabled = Renderer->GetIsEnabled() ? EnabledSuffix : DisabledSuffix;
            const FString Header = FString::Printf(
                TEXT("renderer %s @%d%s"),
                *ShortClassName(Renderer->GetClass()),
                Index,
                Enabled);
            AppendReflectedBlock(Emitter, Header, Renderer);
        }
    }

    void AppendSimStages(FNIRTextEmitter& Emitter, const FVersionedNiagaraEmitterData* EmitterData)
    {
        if (!EmitterData)
        {
            return;
        }
        const TArray<UNiagaraSimulationStageBase*>& Stages = EmitterData->GetSimulationStages();
        for (int32 Index = 0; Index < Stages.Num(); ++Index)
        {
            UNiagaraSimulationStageBase* Stage = Stages[Index];
            if (!Stage)
            {
                continue;
            }
            const FString Header = FString::Printf(
                TEXT("simStage %s @%d"),
                *NIRTextEmitter::FormatNameToken(Stage->GetName()),
                Index);
            Emitter.EnterScope(Header + TEXT(" "));
            for (const FString& Field : BuildReflectedFields(Stage))
            {
                Emitter.AppendLine(Field);
            }
            EmitScriptGraphScope(Stage->Script, Emitter);
            Emitter.ExitScope();
        }
    }

    void AppendEventHandlers(FNIRTextEmitter& Emitter, const FVersionedNiagaraEmitterData* EmitterData)
    {
        if (!EmitterData)
        {
            return;
        }
        const TArray<FNiagaraEventScriptProperties>& Handlers = EmitterData->GetEventHandlers();
        if (Handlers.IsEmpty())
        {
            return;
        }
        // FNiagaraEventScriptProperties is a USTRUCT; use reflected emit on the struct directly.
        FReflectedFieldEmitOptions Options;
        Options.FieldSeparator = TEXT(": ");
        Options.bEmitArraysAsBracketList = true;
        UScriptStruct* Struct = FNiagaraEventScriptProperties::StaticStruct();
        const auto SkipStruct = [](const FStructProperty*) { return false; };
        const auto SkipName = [](FName) { return false; };

        for (int32 Index = 0; Index < Handlers.Num(); ++Index)
        {
            const FNiagaraEventScriptProperties& Handler = Handlers[Index];
            const FString Header = FString::Printf(
                TEXT("eventHandler %s @%d"),
                *NIRTextEmitter::FormatNameToken(Handler.SourceEventName.ToString()),
                Index);
            TArray<FString> Fields;
            FIrTextUtils::AppendReflectedFields(
                Struct,
                &Handler,
                /*Default=*/nullptr,
                /*OwnerForExportText=*/nullptr,
                TSet<FName>(),
                SkipStruct,
                SkipName,
                Options,
                Fields);
            Emitter.EnterScope(Header + TEXT(" "));
            for (const FString& Field : Fields)
            {
                Emitter.AppendLine(Field);
            }
            EmitScriptGraphScope(Handler.Script, Emitter);
            Emitter.ExitScope();
        }
    }

    void AppendEmitterBody(FNIRTextEmitter& Emitter, const UNiagaraEmitter* EmitterAsset)
    {
        if (!EmitterAsset)
        {
            return;
        }
        const FVersionedNiagaraEmitterData* EmitterData = EmitterAsset->GetLatestEmitterData();
        if (!EmitterData)
        {
            Emitter.AppendLine(TEXT("# emitter has no latest version data"));
            return;
        }
        Emitter.AppendLine(FString::Printf(
            TEXT("simTarget: %s"),
            *NIRTextEmitter::FormatNameToken(StaticEnum<ENiagaraSimTarget>()->GetNameStringByValue((int64)EmitterData->SimTarget))));
        AppendEmitterFlags(Emitter, *EmitterData);
        AppendRenderers(Emitter, EmitterData);
        AppendParameterStoreValues(
            Emitter,
            EmitterData->RendererBindings,
            TEXT("rendererBindings"),
            EmitterAsset,
            TEXT("binding"));
        AppendRapidIterationStore(Emitter, EmitterData->EmitterSpawnScriptProps.Script, TEXT("emitterSpawnRapidIteration"), EmitterAsset);
        AppendRapidIterationStore(Emitter, EmitterData->EmitterUpdateScriptProps.Script, TEXT("emitterUpdateRapidIteration"), EmitterAsset);
        AppendRapidIterationStore(Emitter, EmitterData->SpawnScriptProps.Script, TEXT("particleSpawnRapidIteration"), EmitterAsset);
        AppendRapidIterationStore(Emitter, EmitterData->UpdateScriptProps.Script, TEXT("particleUpdateRapidIteration"), EmitterAsset);
        AppendStack(Emitter, TEXT("EmitterSpawn"), EmitterData->EmitterSpawnScriptProps.Script, EmitterData->SimTarget);
        EmitScriptGraphScope(EmitterData->EmitterSpawnScriptProps.Script, Emitter);
        AppendStack(Emitter, TEXT("EmitterUpdate"), EmitterData->EmitterUpdateScriptProps.Script, EmitterData->SimTarget);
        EmitScriptGraphScope(EmitterData->EmitterUpdateScriptProps.Script, Emitter);
        AppendStack(Emitter, TEXT("ParticleSpawn"), EmitterData->SpawnScriptProps.Script, EmitterData->SimTarget);
        EmitScriptGraphScope(EmitterData->SpawnScriptProps.Script, Emitter);
        AppendStack(Emitter, TEXT("ParticleUpdate"), EmitterData->UpdateScriptProps.Script, EmitterData->SimTarget);
        EmitScriptGraphScope(EmitterData->UpdateScriptProps.Script, Emitter);
        AppendSimStages(Emitter, EmitterData);
        AppendEventHandlers(Emitter, EmitterData);
        EmitScriptGraphScope(EmitterData->GetGPUComputeScript(), Emitter);
    }

    void EmitSystem(FNIRTextEmitter& Emitter, UNiagaraSystem& System)
    {
        Emitter.EnterScope(FString::Printf(TEXT("system %s "), *PathQuoted(&System)));

        AppendSystemFlags(Emitter, System);

        // System-level user-exposed parameters.
        AppendParameterStoreValues(Emitter, System.GetExposedParameters(), TEXT("user"), &System, TEXT("param"));
        AppendRapidIterationStore(Emitter, System.GetSystemSpawnScript(), TEXT("systemSpawnRapidIteration"), &System);
        AppendRapidIterationStore(Emitter, System.GetSystemUpdateScript(), TEXT("systemUpdateRapidIteration"), &System);

        AppendStack(Emitter, TEXT("SystemSpawn"), System.GetSystemSpawnScript(), ENiagaraSimTarget::CPUSim);
        EmitScriptGraphScope(System.GetSystemSpawnScript(), Emitter);
        AppendStack(Emitter, TEXT("SystemUpdate"), System.GetSystemUpdateScript(), ENiagaraSimTarget::CPUSim);
        EmitScriptGraphScope(System.GetSystemUpdateScript(), Emitter);

        for (const FNiagaraEmitterHandle& Handle : System.GetEmitterHandles())
        {
            const UNiagaraEmitter* EmitterAsset = Handle.GetInstance().Emitter;
            const TCHAR* Enabled = Handle.GetIsEnabled() ? EnabledSuffix : DisabledSuffix;
            Emitter.EnterScope(FString::Printf(
                TEXT("emitter %s%s "),
                *NIRTextEmitter::FormatNameToken(Handle.GetName().ToString()),
                Enabled));
            AppendEmitterHandleState(Emitter, Handle);
            AppendEmitterBody(Emitter, EmitterAsset);
            Emitter.ExitScope();
        }

        Emitter.ExitScope();
    }

    void EmitEmitterStandalone(FNIRTextEmitter& Emitter, UNiagaraEmitter& EmitterAsset)
    {
        Emitter.EnterScope(FString::Printf(TEXT("emitter %s "), *PathQuoted(&EmitterAsset)));
        AppendEmitterBody(Emitter, &EmitterAsset);
        Emitter.ExitScope();
    }

    void EmitScriptPlaceholder(FNIRTextEmitter& Emitter, UNiagaraScript& Script)
    {
        Emitter.EnterScope(FString::Printf(TEXT("script %s "), *PathQuoted(&Script)));
        EmitScriptGraphScope(&Script, Emitter);
        Emitter.ExitScope();
    }
}

FNIRResult NIRDecompiler::BuildNiagaraIrText(UObject* NiagaraAsset)
{
    if (!NiagaraAsset)
    {
        return FNIRResult::MakeError(TEXT("NIR: asset is null."));
    }

    FNIRResult Result;
    Result.bSuccess = true;
    FNIRTextEmitter Emitter(&Result.Warnings);

    if (UNiagaraSystem* System = Cast<UNiagaraSystem>(NiagaraAsset))
    {
        EmitSystem(Emitter, *System);
    }
    else if (UNiagaraEmitter* EmitterAsset = Cast<UNiagaraEmitter>(NiagaraAsset))
    {
        EmitEmitterStandalone(Emitter, *EmitterAsset);
    }
    else if (UNiagaraScript* Script = Cast<UNiagaraScript>(NiagaraAsset))
    {
        EmitScriptPlaceholder(Emitter, *Script);
    }
    else
    {
        return FNIRResult::MakeError(TEXT("NIR: unsupported asset type"));
    }

    Result.Text = Emitter.ToString();
    return Result;
}
