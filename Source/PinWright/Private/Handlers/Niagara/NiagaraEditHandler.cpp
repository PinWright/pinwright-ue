// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Compat/EngineVersionCompat.h"
#include "Handlers/Niagara/NiagaraEditTypes.h"
#include "Handlers/Niagara/NiagaraJsonHelpers.h"
#include "Handlers/Niagara/NiagaraModuleInputDataInterface.h"
#include "Handlers/Niagara/NiagaraParameterTypeResolver.h"
#include "Handlers/Niagara/NiagaraResetModuleInputHelpers.h"
#include "Math/Float16.h"
#include "Utils/AssetUtils.h"
#include "Handlers/Niagara/NiagaraGraphResetUtils.h"
#include "Handlers/Niagara/NiagaraInstanceUtils.h"
#include "Handlers/Niagara/NiagaraRapidIteration.h"
#include "Utils/JsonUtils.h"
#include "Utils/PropertyUtils.h"

#include "NiagaraCommon.h"
#include "NiagaraEffectType.h"
#include "NiagaraEmitter.h"
#include "NiagaraGraph.h"
#include "NiagaraNode.h"
#include "NiagaraParameterStore.h"
#include "NiagaraUserRedirectionParameterStore.h"
#include "NiagaraRendererProperties.h"
#include "NiagaraNodeFunctionCall.h"
#include "NiagaraNodeInput.h"
#include "NiagaraNodeOutput.h"
#include "NiagaraNodeParameterMapSet.h"
#include "NiagaraNodeStaticSwitch.h"
#include "NiagaraPlatformSet.h"
#include "NiagaraScript.h"
#include "NiagaraScriptSource.h"
#include "NiagaraSystem.h"
#include "Misc/EngineVersionComparison.h"
#include "NiagaraTypes.h"
#include "UObject/StructOnScope.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_Niagara.h"
#include "ScopedTransaction.h"
#include "UObject/Package.h"
#include "UObject/SoftObjectPath.h"
#include "ViewModels/Stack/NiagaraParameterHandle.h"
#include "ViewModels/Stack/NiagaraStackGraphUtilities.h"

namespace
{
    using NiagaraJsonHelpers::SendNiagaraEditError;

    struct FLocalStackNodeGroup
    {
        TArray<UNiagaraNode*> StartNodes;
        UNiagaraNode* EndNode = nullptr;
    };

    double GetPinArrayNumber(const TSharedPtr<FJsonValue>& Value, int32 Index, double DefaultValue = 0.0)
    {
        if (!Value.IsValid() || Value->Type != EJson::Array || !Value->AsArray().IsValidIndex(Index))
        {
            return DefaultValue;
        }
        const TSharedPtr<FJsonValue>& Element = Value->AsArray()[Index];
        return Element.IsValid() && Element->Type == EJson::Number ? Element->AsNumber() : DefaultValue;
    }

    double GetPinObjectNumber(const TSharedPtr<FJsonValue>& Value, const TCHAR* Field, double DefaultValue = 0.0)
    {
        if (!Value.IsValid() || Value->Type != EJson::Object)
        {
            return DefaultValue;
        }
        double Number = DefaultValue;
        Value->AsObject()->TryGetNumberField(Field, Number);
        return Number;
    }

    FString JsonValueToPinDefaultString(const TSharedPtr<FJsonValue>& Value)
    {
        if (!Value.IsValid())
        {
            return FString();
        }
        if (Value->Type == EJson::String)
        {
            return Value->AsString();
        }
        if (Value->Type == EJson::Boolean)
        {
            return Value->AsBool() ? TEXT("true") : TEXT("false");
        }
        if (Value->Type == EJson::Number)
        {
            return FString::SanitizeFloat(Value->AsNumber());
        }
        if (Value->Type == EJson::Array)
        {
            const TArray<TSharedPtr<FJsonValue>>& Array = Value->AsArray();
            if (Array.Num() >= 4)
            {
                return FString::Printf(
                    TEXT("(X=%s,Y=%s,Z=%s,W=%s)"),
                    *FString::SanitizeFloat(GetPinArrayNumber(Value, 0)),
                    *FString::SanitizeFloat(GetPinArrayNumber(Value, 1)),
                    *FString::SanitizeFloat(GetPinArrayNumber(Value, 2)),
                    *FString::SanitizeFloat(GetPinArrayNumber(Value, 3)));
            }
            if (Array.Num() >= 3)
            {
                return FString::Printf(
                    TEXT("(X=%s,Y=%s,Z=%s)"),
                    *FString::SanitizeFloat(GetPinArrayNumber(Value, 0)),
                    *FString::SanitizeFloat(GetPinArrayNumber(Value, 1)),
                    *FString::SanitizeFloat(GetPinArrayNumber(Value, 2)));
            }
            // A 2-element array is an FVector2D (e.g. the Sprite Size input on
            // Initialize Particle). Without this branch it fell through to the
            // empty-string return below and InferNiagaraInputType rejected it.
            if (Array.Num() >= 2)
            {
                return FString::Printf(
                    TEXT("(X=%s,Y=%s)"),
                    *FString::SanitizeFloat(GetPinArrayNumber(Value, 0)),
                    *FString::SanitizeFloat(GetPinArrayNumber(Value, 1)));
            }
        }
        if (Value->Type == EJson::Object)
        {
            const TSharedPtr<FJsonObject> Object = Value->AsObject();
            if (Object.IsValid() && Object->HasField(TEXT("r")) && Object->HasField(TEXT("g")) && Object->HasField(TEXT("b")))
            {
                return FString::Printf(
                    TEXT("(R=%s,G=%s,B=%s,A=%s)"),
                    *FString::SanitizeFloat(GetPinObjectNumber(Value, TEXT("r"))),
                    *FString::SanitizeFloat(GetPinObjectNumber(Value, TEXT("g"))),
                    *FString::SanitizeFloat(GetPinObjectNumber(Value, TEXT("b"))),
                    *FString::SanitizeFloat(GetPinObjectNumber(Value, TEXT("a"), 1.0)));
            }
            if (Object.IsValid() && Object->HasField(TEXT("x")) && Object->HasField(TEXT("y")) && Object->HasField(TEXT("z")))
            {
                if (Object->HasField(TEXT("w")))
                {
                    return FString::Printf(
                        TEXT("(X=%s,Y=%s,Z=%s,W=%s)"),
                        *FString::SanitizeFloat(GetPinObjectNumber(Value, TEXT("x"))),
                        *FString::SanitizeFloat(GetPinObjectNumber(Value, TEXT("y"))),
                        *FString::SanitizeFloat(GetPinObjectNumber(Value, TEXT("z"))),
                        *FString::SanitizeFloat(GetPinObjectNumber(Value, TEXT("w"))));
                }
                return FString::Printf(
                    TEXT("(X=%s,Y=%s,Z=%s)"),
                    *FString::SanitizeFloat(GetPinObjectNumber(Value, TEXT("x"))),
                    *FString::SanitizeFloat(GetPinObjectNumber(Value, TEXT("y"))),
                    *FString::SanitizeFloat(GetPinObjectNumber(Value, TEXT("z"))));
            }
            // An {x,y}-only object (no z) is an FVector2D. Checked after the
            // {x,y,z} case above so a 3-component object still maps to Vec3/Vec4.
            if (Object.IsValid() && Object->HasField(TEXT("x")) && Object->HasField(TEXT("y")))
            {
                return FString::Printf(
                    TEXT("(X=%s,Y=%s)"),
                    *FString::SanitizeFloat(GetPinObjectNumber(Value, TEXT("x"))),
                    *FString::SanitizeFloat(GetPinObjectNumber(Value, TEXT("y"))));
            }
        }
        return FString();
    }

    bool TryReadNumberArray(const TSharedPtr<FJsonValue>& Value, int32 ExpectedCount, TArray<float>& OutNumbers)
    {
        OutNumbers.Reset();
        if (!Value.IsValid() || Value->Type != EJson::Array || Value->AsArray().Num() != ExpectedCount)
        {
            return false;
        }

        OutNumbers.Reserve(ExpectedCount);
        for (const TSharedPtr<FJsonValue>& Element : Value->AsArray())
        {
            if (!Element.IsValid() || Element->Type != EJson::Number)
            {
                OutNumbers.Reset();
                return false;
            }
            const double Number = Element->AsNumber();
            const float FloatNumber = static_cast<float>(Number);
            if (!FMath::IsFinite(Number) || !FMath::IsFinite(FloatNumber))
            {
                OutNumbers.Reset();
                return false;
            }
            OutNumbers.Add(FloatNumber);
        }
        return true;
    }

    // UE's Niagara quaternion pin codec serializes each component with three decimal
    // places. Allow that documented quantization when verifying the schema round-trip.
    constexpr float QuatPinDefaultQuantizationTolerance = 0.00051f;

    FNiagaraEditError EncodeTypedModuleInputLiteral(
        const FString& InputName,
        const FNiagaraTypeDefinition& DeclaredType,
        const TSharedPtr<FJsonValue>& Value,
        bool& bOutHandled,
        FString& OutDefaultValue,
        FNiagaraVariable& OutExpectedValue)
    {
        OutExpectedValue = FNiagaraVariable();
        // A string is already a pin-default literal. Preserve the existing override-pin
        // path for it; typed handling below is for fresh numeric Matrix/Quat values, where
        // inferring Vec4 would otherwise create a mistyped override.
        if (Value.IsValid() && Value->Type == EJson::String)
        {
            bOutHandled = false;
            return FNiagaraEditError();
        }

        bOutHandled = true;
        TArray<float> Numbers;
        if (DeclaredType == FNiagaraTypeDefinition::GetMatrix4Def())
        {
            if (!TryReadNumberArray(Value, 16, Numbers))
            {
                return FNiagaraEditError::Make(TEXT("INVALID_VALUE"), FString::Printf(
                    TEXT("Module input '%s' has declared type '%s' and requires exactly 16 finite numbers: [m00,m01,m02,m03, m10,m11,m12,m13, m20,m21,m22,m23, m30,m31,m32,m33]."),
                    *InputName,
                    *DeclaredType.GetName()));
            }

            FMatrix44f Matrix(ForceInitToZero);
            for (int32 Row = 0; Row < 4; ++Row)
            {
                for (int32 Column = 0; Column < 4; ++Column)
                {
                    Matrix.M[Row][Column] = Numbers[Row * 4 + Column];
                }
            }
            FNiagaraMatrix NiagaraMatrix;
            NiagaraMatrix.Row0 = FVector4f(Matrix.M[0][0], Matrix.M[0][1], Matrix.M[0][2], Matrix.M[0][3]);
            NiagaraMatrix.Row1 = FVector4f(Matrix.M[1][0], Matrix.M[1][1], Matrix.M[1][2], Matrix.M[1][3]);
            NiagaraMatrix.Row2 = FVector4f(Matrix.M[2][0], Matrix.M[2][1], Matrix.M[2][2], Matrix.M[2][3]);
            NiagaraMatrix.Row3 = FVector4f(Matrix.M[3][0], Matrix.M[3][1], Matrix.M[3][2], Matrix.M[3][3]);
            OutExpectedValue = FNiagaraVariable(DeclaredType, FName(*InputName));
            OutExpectedValue.SetData(reinterpret_cast<const uint8*>(&NiagaraMatrix));
            if (!GetDefault<UEdGraphSchema_Niagara>()->TryGetPinDefaultValueFromNiagaraVariable(OutExpectedValue, OutDefaultValue))
            {
                return FNiagaraEditError::Make(TEXT("INVALID_INPUT_VALUE"), FString::Printf(
                    TEXT("Module input '%s' declared as '%s' has no Niagara schema pin-default encoder in this engine version."),
                    *InputName,
                    *DeclaredType.GetName()));
            }
            return FNiagaraEditError();
        }

        if (DeclaredType == FNiagaraTypeDefinition::GetQuatDef())
        {
            if (Value.IsValid() && Value->Type == EJson::Object)
            {
                const TSharedPtr<FJsonObject> Object = Value->AsObject();
                double Component = 0.0;
                for (const TCHAR* Field : {TEXT("x"), TEXT("y"), TEXT("z"), TEXT("w")})
                {
                    if (!Object.IsValid() || !Object->TryGetNumberField(Field, Component))
                    {
                        Numbers.Reset();
                        break;
                    }
                    const float FloatComponent = static_cast<float>(Component);
                    if (!FMath::IsFinite(Component) || !FMath::IsFinite(FloatComponent))
                    {
                        Numbers.Reset();
                        break;
                    }
                    Numbers.Add(FloatComponent);
                }
            }
            else
            {
                TryReadNumberArray(Value, 4, Numbers);
            }

            if (Numbers.Num() != 4)
            {
                return FNiagaraEditError::Make(TEXT("INVALID_VALUE"), FString::Printf(
                    TEXT("Module input '%s' has declared type '%s' and requires exactly four finite numbers as [x,y,z,w] or {x,y,z,w}."),
                    *InputName,
                    *DeclaredType.GetName()));
            }

            FQuat4f Quat(Numbers[0], Numbers[1], Numbers[2], Numbers[3]);
            if (Quat.SizeSquared() <= UE_SMALL_NUMBER)
            {
                return FNiagaraEditError::Make(TEXT("INVALID_VALUE"), FString::Printf(
                    TEXT("Module input '%s' has declared type '%s' and requires a non-zero quaternion that can be normalized."),
                    *InputName,
                    *DeclaredType.GetName()));
            }
            Quat.Normalize();
            OutExpectedValue = FNiagaraVariable(DeclaredType, FName(*InputName));
            OutExpectedValue.SetData(reinterpret_cast<const uint8*>(&Quat));
            if (!GetDefault<UEdGraphSchema_Niagara>()->TryGetPinDefaultValueFromNiagaraVariable(OutExpectedValue, OutDefaultValue))
            {
                return FNiagaraEditError::Make(TEXT("INVALID_INPUT_VALUE"), FString::Printf(
                    TEXT("Module input '%s' declared as '%s' has no Niagara schema pin-default encoder in this engine version."),
                    *InputName,
                    *DeclaredType.GetName()));
            }
            return FNiagaraEditError();
        }

        bOutHandled = false;
        return FNiagaraEditError();
    }

    bool VerifyTypedModuleInputPin(
        const UEdGraphPin& Pin,
        const FNiagaraVariable& ExpectedValue)
    {
        const FNiagaraVariable ActualValue = UEdGraphSchema_Niagara::PinToNiagaraVariable(&Pin, true);
        if (ActualValue.GetType() != ExpectedValue.GetType() || !ActualValue.IsDataAllocated() || !ExpectedValue.IsDataAllocated())
        {
            return false;
        }

        if (ExpectedValue.GetType() == FNiagaraTypeDefinition::GetMatrix4Def())
        {
            const FNiagaraMatrix& Expected = *reinterpret_cast<const FNiagaraMatrix*>(ExpectedValue.GetData());
            const FNiagaraMatrix& Actual = *reinterpret_cast<const FNiagaraMatrix*>(ActualValue.GetData());
            const FVector4f* ExpectedRows[] = {&Expected.Row0, &Expected.Row1, &Expected.Row2, &Expected.Row3};
            const FVector4f* ActualRows[] = {&Actual.Row0, &Actual.Row1, &Actual.Row2, &Actual.Row3};
            for (int32 Row = 0; Row < 4; ++Row)
            {
                for (int32 Column = 0; Column < 4; ++Column)
                {
                    if (!FMath::IsNearlyEqual((*ExpectedRows[Row])[Column], (*ActualRows[Row])[Column]))
                    {
                        return false;
                    }
                }
            }
            return true;
        }

        if (ExpectedValue.GetType() == FNiagaraTypeDefinition::GetQuatDef())
        {
            const FQuat4f& Expected = *reinterpret_cast<const FQuat4f*>(ExpectedValue.GetData());
            const FQuat4f& Actual = *reinterpret_cast<const FQuat4f*>(ActualValue.GetData());
            return FMath::IsNearlyEqual(Expected.X, Actual.X, QuatPinDefaultQuantizationTolerance)
                && FMath::IsNearlyEqual(Expected.Y, Actual.Y, QuatPinDefaultQuantizationTolerance)
                && FMath::IsNearlyEqual(Expected.Z, Actual.Z, QuatPinDefaultQuantizationTolerance)
                && FMath::IsNearlyEqual(Expected.W, Actual.W, QuatPinDefaultQuantizationTolerance);
        }

        return false;
    }

    bool InferNiagaraInputType(const TSharedPtr<FJsonValue>& Value, FNiagaraTypeDefinition& OutType, FString& OutDefaultValue)
    {
        OutDefaultValue = JsonValueToPinDefaultString(Value);
        if (!Value.IsValid() || OutDefaultValue.IsEmpty())
        {
            return false;
        }
        if (Value->Type == EJson::Boolean)
        {
            OutType = FNiagaraTypeDefinition::GetBoolDef();
            return true;
        }
        if (Value->Type == EJson::Number)
        {
            OutType = FNiagaraTypeDefinition::GetFloatDef();
            return true;
        }
        if (Value->Type == EJson::Array)
        {
            const int32 Num = Value->AsArray().Num();
            // Thresholds match JsonValueToPinDefaultString exactly: arrays under
            // 2 elements have no vector spelling, so reject them here rather than
            // typing them Vec2 and relying on the empty-string guard above.
            if (Num < 2)
            {
                return false;
            }
            OutType = Num >= 4 ? FNiagaraTypeDefinition::GetVec4Def()
                : Num >= 3 ? FNiagaraTypeDefinition::GetVec3Def()
                : FNiagaraTypeDefinition::GetVec2Def();
            return true;
        }
        if (Value->Type == EJson::Object)
        {
            const TSharedPtr<FJsonObject> Object = Value->AsObject();
            if (Object.IsValid() && Object->HasField(TEXT("r")) && Object->HasField(TEXT("g")) && Object->HasField(TEXT("b")))
            {
                OutType = FNiagaraTypeDefinition::GetColorDef();
                return true;
            }
            if (Object.IsValid() && Object->HasField(TEXT("x")) && Object->HasField(TEXT("y")) && Object->HasField(TEXT("z")))
            {
                OutType = Object->HasField(TEXT("w")) ? FNiagaraTypeDefinition::GetVec4Def() : FNiagaraTypeDefinition::GetVec3Def();
                return true;
            }
            // {x,y}-only object → FVector2D. Mirrors the Vec2 case added to
            // JsonValueToPinDefaultString; checked after {x,y,z} above.
            if (Object.IsValid() && Object->HasField(TEXT("x")) && Object->HasField(TEXT("y")))
            {
                OutType = FNiagaraTypeDefinition::GetVec2Def();
                return true;
            }
        }
        return false;
    }

    bool TryResolveStackScriptUsage(const FNiagaraResolvedTarget& Target, const FString& ScriptUsageText, ENiagaraScriptUsage& OutUsage)
    {
        if (Target.EmitterData)
        {
            ENiagaraScriptUsage ParsedUsage = ENiagaraScriptUsage::ParticleUpdateScript;
            if (NiagaraEdit::TryParseStackScriptUsageAlias(ScriptUsageText, ParsedUsage))
            {
                switch (ParsedUsage)
                {
                case ENiagaraScriptUsage::EmitterSpawnScript:
                case ENiagaraScriptUsage::EmitterUpdateScript:
                case ENiagaraScriptUsage::ParticleSpawnScript:
                case ENiagaraScriptUsage::ParticleUpdateScript:
                    OutUsage = ParsedUsage;
                    return true;
                default:
                    break;
                }
            }
            if (ScriptUsageText.TrimStartAndEnd().Equals(TEXT("Spawn"), ESearchCase::IgnoreCase))
            {
                OutUsage = ENiagaraScriptUsage::ParticleSpawnScript;
                return true;
            }
            OutUsage = ENiagaraScriptUsage::ParticleUpdateScript;
            return true;
        }

        if (Target.System)
        {
            ENiagaraScriptUsage ParsedUsage = ENiagaraScriptUsage::SystemSpawnScript;
            if (NiagaraEdit::TryParseStackScriptUsageAlias(ScriptUsageText, ParsedUsage)
                && ParsedUsage == ENiagaraScriptUsage::SystemUpdateScript)
            {
                OutUsage = ENiagaraScriptUsage::SystemUpdateScript;
                return true;
            }
            if (ScriptUsageText.TrimStartAndEnd().Equals(TEXT("Update"), ESearchCase::IgnoreCase))
            {
                OutUsage = ENiagaraScriptUsage::SystemUpdateScript;
                return true;
            }
            OutUsage = ENiagaraScriptUsage::SystemSpawnScript;
            return true;
        }
        return false;
    }

    UEdGraphPin* FindGraphPinByName(UEdGraphNode* Node, const FString& PinName)
    {
        if (!Node || PinName.IsEmpty())
        {
            return nullptr;
        }
        for (UEdGraphPin* Pin : Node->Pins)
        {
            if (Pin
                && (Pin->PinId.ToString().Equals(PinName, ESearchCase::IgnoreCase)
                    || Pin->PinName.ToString().Equals(PinName, ESearchCase::IgnoreCase)
                    || Pin->GetDisplayName().ToString().Equals(PinName, ESearchCase::IgnoreCase)))
            {
                return Pin;
            }
        }
        return nullptr;
    }

    UEdGraphNode* FindGraphNodeById(UNiagaraGraph* Graph, const FString& NodeId)
    {
        if (!Graph || NodeId.IsEmpty())
        {
            return nullptr;
        }
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (Node
                && (Node->NodeGuid.ToString().Equals(NodeId, ESearchCase::IgnoreCase)
                    || Node->GetName().Equals(NodeId, ESearchCase::IgnoreCase)
                    || Node->GetNodeTitle(ENodeTitleType::ListView).ToString().Equals(NodeId, ESearchCase::IgnoreCase)))
            {
                return Node;
            }
        }
        return nullptr;
    }

    UEdGraphPin* GetParameterMapPin(UNiagaraNode& Node, EEdGraphPinDirection Direction)
    {
        TArray<UEdGraphPin*> Pins;
        if (Direction == EGPD_Input)
        {
            Node.GetInputPins(Pins);
        }
        else
        {
            Node.GetOutputPins(Pins);
        }
        return PinWrightNiagara::FindParameterMapPin(Pins);
    }

    void MakeGraphLink(UEdGraphPin* PinA, UEdGraphPin* PinB)
    {
        if (!PinA || !PinB)
        {
            return;
        }
        PinA->MakeLinkTo(PinB);
        PinA->GetOwningNode()->PinConnectionListChanged(PinA);
        PinB->GetOwningNode()->PinConnectionListChanged(PinB);
    }

    UNiagaraNodeInput* FindStackInputNode(UNiagaraNodeOutput& OutputNode)
    {
        UNiagaraNode* CurrentNode = &OutputNode;
        while (CurrentNode)
        {
            if (UNiagaraNodeInput* InputNode = Cast<UNiagaraNodeInput>(CurrentNode))
            {
                return InputNode;
            }
            UEdGraphPin* InputPin = GetParameterMapPin(*CurrentNode, EGPD_Input);
            if (!InputPin || InputPin->LinkedTo.Num() != 1)
            {
                return nullptr;
            }
            CurrentNode = Cast<UNiagaraNode>(InputPin->LinkedTo[0]->GetOwningNode());
        }
        return nullptr;
    }

    void GetOrderedModuleNodes(UNiagaraNodeOutput& OutputNode, TArray<UNiagaraNodeFunctionCall*>& OutModuleNodes)
    {
        // Single canonical stack-execution-order walk, shared with the inspect readback
        // (NiagaraDumpBuilder.cpp) so move_module and the dump agree on module order.
        PinWrightNiagara::CollectModuleNodesForOutput(OutputNode, OutModuleNodes);
    }

    // Rewrite NodePosY on the stack's module function-call nodes so they sort, ascending by
    // NodePosY, into ParameterMap chain order. The engine's drag-drop reorder relies on
    // FNiagaraStackGraphUtilities::RelayoutGraph for this, but that helper is not NIAGARAEDITOR_API,
    // so this inlines the minimal equivalent for the module nodes — the only nodes the stack readback
    // (NiagaraDumpBuilder::AddGraphStackModules) and the editor stack view sort on. RelayoutGraph lays
    // the output-adjacent module out at the smallest Y (closest to the OutputNode) and each module
    // further up the chain at a larger Y; GetOrderedModuleNodes returns top-of-stack first (furthest
    // from the output), so we assign descending Y across that list to match the engine's convention.
    void RelayoutModuleNodePositions(UNiagaraNodeOutput& OutputNode)
    {
        TArray<UNiagaraNodeFunctionCall*> ModuleNodes;
        GetOrderedModuleNodes(OutputNode, ModuleNodes);

        // Mirror the engine's per-level vertical stride (NiagaraStackGraphUtilities.cpp RelayoutGraph
        // uses YDistance == 50). Exact spacing is immaterial to the sort; only strict monotonicity is.
        constexpr int32 YStride = 50;
        const int32 Count = ModuleNodes.Num();
        for (int32 Index = 0; Index < Count; ++Index)
        {
            if (UNiagaraNodeFunctionCall* ModuleNode = ModuleNodes[Index])
            {
                ModuleNode->Modify();
                ModuleNode->NodePosY = (Count - 1 - Index) * YStride;
            }
        }
    }

    void GetGroupNodesRecursive(const TArray<UNiagaraNode*>& CurrentStartNodes, UNiagaraNode* EndNode, TArray<UNiagaraNode*>& OutAllNodes)
    {
        for (UNiagaraNode* CurrentStartNode : CurrentStartNodes)
        {
            if (!CurrentStartNode || OutAllNodes.Contains(CurrentStartNode))
            {
                continue;
            }

            OutAllNodes.Add(CurrentStartNode);
            if (UEdGraphPin* ParameterMapInputPin = GetParameterMapPin(*CurrentStartNode, EGPD_Input))
            {
                TArray<UEdGraphPin*> InputPins;
                CurrentStartNode->GetInputPins(InputPins);
                for (UEdGraphPin* InputPin : InputPins)
                {
                    if (InputPin == ParameterMapInputPin)
                    {
                        continue;
                    }
                    for (UEdGraphPin* LinkedPin : InputPin->LinkedTo)
                    {
                        if (UNiagaraNode* LinkedNode = Cast<UNiagaraNode>(LinkedPin->GetOwningNode()))
                        {
                            OutAllNodes.AddUnique(LinkedNode);
                        }
                    }
                }
            }

            if (CurrentStartNode != EndNode)
            {
                TArray<UNiagaraNode*> LinkedNodes;
                TArray<UEdGraphPin*> OutputPins;
                CurrentStartNode->GetOutputPins(OutputPins);
                for (UEdGraphPin* OutputPin : OutputPins)
                {
                    for (UEdGraphPin* LinkedPin : OutputPin->LinkedTo)
                    {
                        if (UNiagaraNode* LinkedNode = Cast<UNiagaraNode>(LinkedPin->GetOwningNode()))
                        {
                            LinkedNodes.Add(LinkedNode);
                        }
                    }
                }
                GetGroupNodesRecursive(LinkedNodes, EndNode, OutAllNodes);
            }
        }
    }

    void GetStackNodeGroups(UNiagaraNodeOutput& OutputNode, TArray<FLocalStackNodeGroup>& OutGroups)
    {
        if (UNiagaraNodeInput* InputNode = FindStackInputNode(OutputNode))
        {
            FLocalStackNodeGroup InputGroup;
            InputGroup.StartNodes.Add(InputNode);
            InputGroup.EndNode = InputNode;
            OutGroups.Add(InputGroup);
        }
        else
        {
            return;
        }

        TArray<UNiagaraNodeFunctionCall*> ModuleNodes;
        GetOrderedModuleNodes(OutputNode, ModuleNodes);
        for (UNiagaraNodeFunctionCall* ModuleNode : ModuleNodes)
        {
            UEdGraphPin* PreviousOutputPin = GetParameterMapPin(*OutGroups.Last().EndNode, EGPD_Output);
            if (!PreviousOutputPin)
            {
                OutGroups.Reset();
                return;
            }

            FLocalStackNodeGroup ModuleGroup;
            for (UEdGraphPin* LinkedPin : PreviousOutputPin->LinkedTo)
            {
                if (UNiagaraNode* StartNode = Cast<UNiagaraNode>(LinkedPin->GetOwningNode()))
                {
                    ModuleGroup.StartNodes.Add(StartNode);
                }
            }
            ModuleGroup.EndNode = ModuleNode;
            OutGroups.Add(ModuleGroup);
        }

        UEdGraphPin* PreviousOutputPin = GetParameterMapPin(*OutGroups.Last().EndNode, EGPD_Output);
        if (!PreviousOutputPin)
        {
            OutGroups.Reset();
            return;
        }

        FLocalStackNodeGroup OutputGroup;
        for (UEdGraphPin* LinkedPin : PreviousOutputPin->LinkedTo)
        {
            if (UNiagaraNode* StartNode = Cast<UNiagaraNode>(LinkedPin->GetOwningNode()))
            {
                OutputGroup.StartNodes.Add(StartNode);
            }
        }
        OutputGroup.EndNode = &OutputNode;
        OutGroups.Add(OutputGroup);
    }

    bool DisconnectStackGroup(const FLocalStackNodeGroup& Group, const FLocalStackNodeGroup& PreviousGroup, const FLocalStackNodeGroup& NextGroup)
    {
        UEdGraphPin* PreviousOutputPin = PreviousGroup.EndNode ? GetParameterMapPin(*PreviousGroup.EndNode, EGPD_Output) : nullptr;
        UEdGraphPin* GroupOutputPin = Group.EndNode ? GetParameterMapPin(*Group.EndNode, EGPD_Output) : nullptr;
        if (!PreviousOutputPin || !GroupOutputPin)
        {
            return false;
        }

        PreviousOutputPin->BreakAllPinLinks(true);
        GroupOutputPin->BreakAllPinLinks(true);

        for (UNiagaraNode* NextStartNode : NextGroup.StartNodes)
        {
            UEdGraphPin* NextInputPin = NextStartNode ? GetParameterMapPin(*NextStartNode, EGPD_Input) : nullptr;
            if (!NextInputPin)
            {
                return false;
            }
            MakeGraphLink(PreviousOutputPin, NextInputPin);
        }
        return true;
    }

    bool ConnectStackGroup(const FLocalStackNodeGroup& Group, const FLocalStackNodeGroup& PreviousGroup, const FLocalStackNodeGroup& NextGroup)
    {
        UEdGraphPin* PreviousOutputPin = PreviousGroup.EndNode ? GetParameterMapPin(*PreviousGroup.EndNode, EGPD_Output) : nullptr;
        UEdGraphPin* GroupOutputPin = Group.EndNode ? GetParameterMapPin(*Group.EndNode, EGPD_Output) : nullptr;
        if (!PreviousOutputPin || !GroupOutputPin)
        {
            return false;
        }

        PreviousOutputPin->BreakAllPinLinks(true);
        for (UNiagaraNode* StartNode : Group.StartNodes)
        {
            UEdGraphPin* StartInputPin = StartNode ? GetParameterMapPin(*StartNode, EGPD_Input) : nullptr;
            if (!StartInputPin)
            {
                return false;
            }
            MakeGraphLink(PreviousOutputPin, StartInputPin);
        }

        for (UNiagaraNode* NextStartNode : NextGroup.StartNodes)
        {
            UEdGraphPin* NextInputPin = NextStartNode ? GetParameterMapPin(*NextStartNode, EGPD_Input) : nullptr;
            if (!NextInputPin)
            {
                return false;
            }
            MakeGraphLink(GroupOutputPin, NextInputPin);
        }
        return true;
    }

    UNiagaraNodeOutput* ResolveStackOutputNode(const FNiagaraResolvedTarget& Target, const FString& ScriptUsageText)
    {
        if (!Target.Graph)
        {
            return nullptr;
        }

        ENiagaraScriptUsage Usage = ENiagaraScriptUsage::ParticleUpdateScript;
        if (TryResolveStackScriptUsage(Target, ScriptUsageText, Usage))
        {
            if (UNiagaraNodeOutput* OutputNode = Target.Graph->FindEquivalentOutputNode(Usage, FGuid()))
            {
                return OutputNode;
            }
        }

        UNiagaraNodeOutput* OnlyOutputNode = nullptr;
        for (UEdGraphNode* Node : Target.Graph->Nodes)
        {
            if (UNiagaraNodeOutput* OutputNode = Cast<UNiagaraNodeOutput>(Node))
            {
                if (OnlyOutputNode)
                {
                    return nullptr;
                }
                OnlyOutputNode = OutputNode;
            }
        }
        return OnlyOutputNode;
    }

    UNiagaraNodeOutput* FindOwningStackOutputNode(const FNiagaraResolvedTarget& Target)
    {
        if (!Target.ModuleNode)
        {
            return nullptr;
        }

        TArray<UNiagaraNode*> NodesToCheck;
        TSet<UNiagaraNode*> NodesSeen;
        NodesToCheck.Add(Target.ModuleNode);
        NodesSeen.Add(Target.ModuleNode);

        while (NodesToCheck.Num() > 0)
        {
            UNiagaraNode* NodeToCheck = NodesToCheck[0];
            NodesToCheck.RemoveAt(0);

            if (UNiagaraNodeOutput* OutputNode = Cast<UNiagaraNodeOutput>(NodeToCheck))
            {
                return OutputNode;
            }

            TArray<UEdGraphPin*> OutputPins;
            NodeToCheck->GetOutputPins(OutputPins);
            for (UEdGraphPin* OutputPin : OutputPins)
            {
                if (!OutputPin)
                {
                    continue;
                }

                for (UEdGraphPin* LinkedPin : OutputPin->LinkedTo)
                {
                    UNiagaraNode* LinkedNiagaraNode = LinkedPin ? Cast<UNiagaraNode>(LinkedPin->GetOwningNode()) : nullptr;
                    if (LinkedNiagaraNode && !NodesSeen.Contains(LinkedNiagaraNode))
                    {
                        NodesSeen.Add(LinkedNiagaraNode);
                        NodesToCheck.Add(LinkedNiagaraNode);
                    }
                }
            }
        }
        return nullptr;
    }

    void NotifyNiagaraGraphChanged(const FNiagaraResolvedTarget& Target)
    {
        if (Target.Graph)
        {
            UEdGraph* Graph = Target.Graph;
            Graph->NotifyGraphChanged();
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

    // Forward declarations for the linked-parameter helpers defined later in this TU —
    // ApplyModuleMutation (below) calls them before their point of definition.
    bool TryGetLinkedParameterRequest(const TSharedPtr<FJsonValue>& Value, FString& OutParameterName);
    bool TryFindModuleStackInput(
        const UNiagaraNodeFunctionCall* ModuleNode,
        const FString& InputName,
        FNiagaraTypeDefinition* OutType,
        TArray<FString>* OutAvailableNames);
    FNiagaraEditError ResolveLinkedParameter(
        UNiagaraSystem* System,
        const FString& ParameterName,
        const FNiagaraTypeDefinition& InputType,
        FNiagaraVariable& OutVariable);
    bool AreNiagaraParameterTypesCompatible(const FNiagaraTypeDefinition& ExistingType, const FNiagaraTypeDefinition& RequestedType);
    // Dynamic-input value mode: value = { dynamicInput: "<ScriptAssetPath>" } assigns a dynamic-input
    // function-call node onto the module input's override pin (the write-side peer of the linked-
    // parameter mode). Helpers are defined later in this TU; forward-declared here for the branch above.
    bool TryGetDynamicInputRequest(const TSharedPtr<FJsonValue>& Value, FString& OutScriptPath);
    UNiagaraScript* LoadDynamicInputScript(const FString& ScriptPath, FNiagaraEditError& OutError);
    FNiagaraTypeDefinition GetDynamicInputOutputType(UNiagaraScript* Script);

    // What a SetModuleInput write displaced from the input's override pin, plus the literal
    // mode's opt-in for displacing it at all.
    //
    // All three value modes (literal / link / dynamicInput) replace whatever the override pin
    // already carried, and all three report it here so the mutation is disclosed rather than
    // inferred from silence. They differ only in whether the replacement needs permission: a
    // literal written over an inbound link (a dynamic-input chain, a parameter binding, a
    // data-interface / object input) would never be read — that pin is not a value slot, the
    // graph reads the link and ignores the pin default — so the literal branch refuses by
    // default and proceeds only on bBreakExistingLink. The link and dynamicInput modes take
    // effect exactly as asked, so they are not gated; see their branches in ApplyModuleMutation.
    //
    // Out fields stay empty when nothing was displaced. Source names the displaced driver (the
    // linked parameter, or the dynamic-input script path) and is empty for a displaced literal,
    // whose pin-default text is carried in Value instead.
    struct FNiagaraOverrideReplacement
    {
        bool bBreakExistingLink = false; // literal value mode only
        FString ValueMode;
        FString Source;
        FString Value;
    };

    // Classify what the input's override pin currently carries, record it, then tear the pin
    // and its orphaned upstream chain down.
    //
    // The link and dynamicInput modes must clear before they wire (the engine's
    // SetLinkedParameterValueForFunctionInput checkf()s a pin that still has a link), so the
    // classification has to run first — and it runs the same NiagaraEdit::ClassifyModuleInputBindings
    // walk that feeds the inspect / asset.dump `valueMode` readback, so what a write reports as
    // displaced and what a later inspect reports for that input cannot drift apart.
    //
    // OutReplacement is left untouched when the input had no override pin at all: nothing was
    // displaced and the response omits replacedOverride.
    void ClearOverrideAndRecordReplacement(
        UNiagaraNodeFunctionCall& ModuleNode,
        const FNiagaraParameterHandle& AliasedInputHandle,
        UNiagaraGraph& Graph,
        FNiagaraOverrideReplacement* OutReplacement)
    {
        if (OutReplacement)
        {
            TMap<FName, NiagaraEdit::FModuleInputBindingInfo> Bindings;
            NiagaraEdit::ClassifyModuleInputBindings(ModuleNode, Bindings);
            if (const NiagaraEdit::FModuleInputBindingInfo* Binding = Bindings.Find(AliasedInputHandle.GetName()))
            {
                OutReplacement->ValueMode = Binding->ValueMode;
                OutReplacement->Source = !Binding->LinkedParameter.IsEmpty()
                    ? Binding->LinkedParameter
                    : Binding->DynamicInputScript;
                OutReplacement->Value = Binding->LiteralValue;
            }
        }
        NiagaraResetModuleInput::ClearModuleInputOverride(ModuleNode, AliasedInputHandle, Graph);
    }

    // One shape for what a module-input verb did to the rapid-iteration constant, so set / reset /
    // clear report the same object and a caller can read all three with one branch. Returns null
    // when the verb never looked (no constant name was resolved), which is how a value mode that
    // has no constant — a link, a dynamic input — omits the field entirely.
    TSharedPtr<FJsonObject> MakeRapidIterationReport(const PinWrightNiagara::FRapidIterationWriteThrough& Report)
    {
        if (Report.ParameterName.IsEmpty())
        {
            return nullptr;
        }
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("parameter"), Report.ParameterName);
        Obj->SetBoolField(TEXT("shadowed"), Report.bShadowed);
        if (!Report.Action.IsEmpty())
        {
            Obj->SetStringField(TEXT("action"), Report.Action);
        }
        if (!Report.PreviousValue.IsEmpty())
        {
            Obj->SetStringField(TEXT("previousValue"), Report.PreviousValue);
        }
        Obj->SetArrayField(TEXT("updatedScopes"), EmitStringArray(Report.UpdatedScopes));
        if (Report.TypeMismatchScopes.Num() > 0)
        {
            // The stored constant disagrees with the pin about the input's type, so its bytes were
            // left alone: reinterpreting them would corrupt the entry. The shadow stands.
            Obj->SetArrayField(TEXT("typeMismatchScopes"), EmitStringArray(Report.TypeMismatchScopes));
        }
        return Obj;
    }

    FNiagaraEditError ApplyModuleMutation(
        const FNiagaraModuleEditPayload& Payload,
        ENiagaraEditOperation Operation,
        FNiagaraResolvedTarget& Target,
        bool bEnabled,
        FString& OutNodeId,
        int32& OutIndex,
        TMap<FNiagaraVariable, FString>* OutOverrideSnapshot = nullptr,
        // When a SetModuleInput value requested a linked parameter, the resolved
        // fully-qualified parameter name is written here (empty otherwise) so the handler
        // can report `linked: true, parameter, parameterType` instead of a literal pinId.
        FString* OutLinkedParameter = nullptr,
        FString* OutLinkedParameterType = nullptr,
        // For a literal SetModuleInput write, the canonical pin-default string actually
        // landed on the override pin (the same DefaultValue handed to TrySetDefaultValue) is
        // written here so the handler can echo the value it wrote — set_module_input writes
        // a graph override pin, NOT the rapid-iteration store the inspect `parameters` aspect
        // reads, so a params-only readback shows the stale template default. Empty for the
        // linked path (which reports `linked`/`parameter` instead).
        FString* OutWrittenValue = nullptr,
        // When a SetModuleInput value assigned a dynamic-input chain, the dynamic-input script
        // path is written here so the handler can report `dynamicInput: "<path>"`.
        FString* OutDynamicInput = nullptr,
        // What the SetModuleInput write displaced from the override pin, plus the literal mode's
        // opt-in for displacing a linked one; see FNiagaraOverrideReplacement above. Null means
        // "refuse a linked pin and report nothing", which is the default for every caller that
        // does not thread the caller's opt-in and the response record through.
        FNiagaraOverrideReplacement* InOutReplacedOverride = nullptr,
        // For a literal SetModuleInput write: what the same value found and changed in the
        // rapid-iteration stores, so the handler can report the shadowing constant it reconciled.
        // Null skips the write-through entirely, which is what every caller other than
        // set_module_input wants — none of them writes a module-input literal.
        PinWrightNiagara::FRapidIterationWriteThrough* OutRapidIteration = nullptr)
    {
        OutIndex = INDEX_NONE;

        if (Operation == ENiagaraEditOperation::AddModule)
        {
            UNiagaraNodeOutput* OutputNode = ResolveStackOutputNode(Target, Payload.Target.ScriptUsage);
            if (!OutputNode)
            {
                return FNiagaraEditError::Make(TEXT("OUTPUT_NODE_NOT_FOUND"), TEXT("Could not resolve Niagara stack output node for the requested script usage."));
            }

            UNiagaraScript* ModuleScript = LoadObject<UNiagaraScript>(nullptr, *Payload.ModulePath);
            if (!ModuleScript)
            {
                return FNiagaraEditError::Make(TEXT("MODULE_SCRIPT_NOT_FOUND"), FString::Printf(TEXT("Could not load Niagara module script '%s'."), *Payload.ModulePath));
            }

            const int32 TargetIndex = Payload.Target.ToIndex == INDEX_NONE ? INDEX_NONE : Payload.Target.ToIndex;
            UNiagaraNodeFunctionCall* NewModule = FNiagaraStackGraphUtilities::AddScriptModuleToStack(ModuleScript, *OutputNode, TargetIndex, ModuleScript->GetName());
            if (!NewModule)
            {
                return FNiagaraEditError::Make(TEXT("MODULE_ADD_FAILED"), FString::Printf(TEXT("Failed to add module '%s' to the Niagara stack."), *Payload.ModulePath));
            }

            Target.Node = NewModule;
            Target.ModuleNode = NewModule;
            Target.ReflectedObject = NewModule;
            OutNodeId = NewModule->NodeGuid.ToString();
            TArray<UNiagaraNodeFunctionCall*> Modules;
            GetOrderedModuleNodes(*OutputNode, Modules);
            OutIndex = Modules.IndexOfByKey(NewModule);
            return FNiagaraEditError();
        }

        if (!Target.Graph || !Target.ModuleNode)
        {
            return FNiagaraEditError::Make(TEXT("MODULE_NOT_FOUND"), TEXT("Module target was not resolved."));
        }

        UNiagaraNodeOutput* OutputNode = nullptr;
        // With scriptUsage omitted the owning stack is inferred from the resolved module node, since
        // ResolveStackOutputNode would otherwise default to ParticleUpdateScript and wrongly report
        // INVALID_STACK for a module living in any other stage (EmitterUpdate / ParticleSpawn / SystemX).
        if (Payload.Target.ScriptUsage.IsEmpty())
        {
            OutputNode = FindOwningStackOutputNode(Target);
        }
        if (!OutputNode)
        {
            OutputNode = ResolveStackOutputNode(Target, Payload.Target.ScriptUsage);
        }
        if (!OutputNode)
        {
            return FNiagaraEditError::Make(TEXT("OUTPUT_NODE_NOT_FOUND"), TEXT("Could not resolve Niagara stack output node for the requested script usage."));
        }

        TArray<FLocalStackNodeGroup> Groups;
        GetStackNodeGroups(*OutputNode, Groups);
        const int32 SourceGroupIndex = Groups.IndexOfByPredicate(
            [&Target](const FLocalStackNodeGroup& Group)
            {
                return Group.EndNode == Target.ModuleNode;
            });
        if (SourceGroupIndex <= 0 || SourceGroupIndex >= Groups.Num() - 1)
        {
            return FNiagaraEditError::Make(TEXT("INVALID_STACK"), FString::Printf(TEXT("Module '%s' is not in a valid stack group."), *Payload.Target.EntryId));
        }

        OutNodeId = Target.ModuleNode->NodeGuid.ToString();
        OutIndex = SourceGroupIndex - 1;

        if (Operation == ENiagaraEditOperation::SetStackEnabled)
        {
            FNiagaraStackGraphUtilities::SetModuleIsEnabled(*Target.ModuleNode, bEnabled);
            return FNiagaraEditError();
        }

        if (Operation == ENiagaraEditOperation::SetModuleInput)
        {
            FNiagaraTypeDefinition DeclaredInputType;
            TArray<FString> AvailableInputNames;
            if (!TryFindModuleStackInput(
                    Target.ModuleNode,
                    Payload.InputName,
                    &DeclaredInputType,
                    &AvailableInputNames))
            {
                const FString Available = AvailableInputNames.Num() > 0
                    ? FString::Printf(TEXT(" Its stack inputs are: %s."), *FString::Join(AvailableInputNames, TEXT(", ")))
                    : FString(TEXT(" It declares no stack inputs."));
                return FNiagaraEditError::Make(TEXT("MODULE_INPUT_NOT_FOUND"),
                    FString::Printf(
                        TEXT("Module '%s' declares no stack input '%s'.%s"),
                        *Target.ModuleNode->GetFunctionName(),
                        *Payload.InputName,
                        *Available));
            }

            FNiagaraParameterHandle InputHandle = Payload.InputName.Contains(TEXT("."))
                ? FNiagaraParameterHandle(FName(*Payload.InputName))
                : FNiagaraParameterHandle::CreateModuleParameterHandle(FName(*Payload.InputName));
            const FNiagaraParameterHandle AliasedInputHandle = FNiagaraParameterHandle::CreateAliasedModuleParameterHandle(InputHandle, Target.ModuleNode);

            // Linked-parameter path: value = { link: "User.WispColor" } binds the input to read
            // from a named parameter rather than writing a literal default. The inverse of the NIR
            // decompiler's "$Namespace.Name" classification (EmitInputValueExpr Case 2). Resolve and
            // validate the parameter, clear any prior override, then wire a parameter-read node into
            // a freshly created override pin via the engine's stack utility.
            FString LinkedParameterName;
            if (TryGetLinkedParameterRequest(Payload.Value, LinkedParameterName))
            {
                FNiagaraVariable LinkedVariable;
                FNiagaraEditError ResolveError =
                    ResolveLinkedParameter(Target.System, LinkedParameterName, DeclaredInputType, LinkedVariable);
                if (ResolveError.HasError())
                {
                    return ResolveError;
                }

                // Type-match the linked parameter against the input's declared type. Skip only when
                // the module input type could not be discovered (declared type invalid) — the engine
                // schema still enforces type at link time in that case. This is meaningful only for
                // the User-store path, where ResolveLinkedParameter returns the store's real type;
                // the non-User path is type-by-construction (it sets the type from DeclaredInputType).
                if (DeclaredInputType.IsValid()
                    && !AreNiagaraParameterTypesCompatible(DeclaredInputType, LinkedVariable.GetType()))
                {
                    return FNiagaraEditError::Make(TEXT("PARAMETER_TYPE_MISMATCH"),
                        FString::Printf(
                            TEXT("Parameter '%s' is type '%s' but module input '%s' expects '%s'."),
                            *LinkedVariable.GetName().ToString(),
                            *LinkedVariable.GetType().GetName(),
                            *Payload.InputName,
                            *DeclaredInputType.GetName()));
                }

                // SetLinkedParameterValueForFunctionInput checkf()s that the override pin has no
                // existing link, so clear any prior override (literal default, dynamic-input chain,
                // or stale link) before creating the fresh pin to wire the parameter read into.
                // Shared with the reset_module_input override-pin path's removal logic.
                //
                // Deliberately NOT gated behind breakExistingLink the way the literal mode is. A
                // literal over a link was never a working operation — it landed a byte the graph
                // does not read — so refusing it costs a caller nothing. A link written over an
                // existing override is the operation the caller asked for and it takes effect, so
                // gating it would refuse the ordinary re-bind (User.A -> User.B) and the idempotent
                // retry of a verb named "set". What it must not be is silent: it deletes an
                // artist's dynamic-input chain or another agent's binding, so record what it took.
                ClearOverrideAndRecordReplacement(*Target.ModuleNode, AliasedInputHandle, *Target.Graph, InOutReplacedOverride);

                const FNiagaraTypeDefinition LinkType = LinkedVariable.GetType();
                UEdGraphPin& OverridePin = FNiagaraStackGraphUtilities::GetOrCreateStackFunctionInputOverridePin(
                    *Target.ModuleNode,
                    AliasedInputHandle,
                    LinkType,
                    FGuid(),
                    FGuid());
                OverridePin.Modify();

                // SetLinkedParameterValueForFunctionInput (FNiagaraVariableBase, TSet<FNiagaraVariableBase>)
                // arrived in UE 5.6, replacing SetLinkedValueHandleForFunctionInput
                // (FNiagaraParameterHandle, TSet<FNiagaraVariable>) on 5.4/5.5. The KnownParameters set
                // is consulted only to pick an equivalent loose/static type when one exists; an empty
                // set selects the requested type verbatim (the link reads LinkedVariable as authored).
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 6, 0)
                const FNiagaraVariableBase LinkedParameter(LinkType, LinkedVariable.GetName());
                const TSet<FNiagaraVariableBase> KnownParameters;
                FNiagaraStackGraphUtilities::SetLinkedParameterValueForFunctionInput(
                    OverridePin,
                    LinkedParameter,
                    KnownParameters,
                    ENiagaraDefaultMode::FailIfPreviouslyNotSet,
                    FGuid());
#else
                const FNiagaraParameterHandle LinkedParameterHandle(LinkedVariable.GetName());
                const TSet<FNiagaraVariable> KnownParameters;
                FNiagaraStackGraphUtilities::SetLinkedValueHandleForFunctionInput(
                    OverridePin,
                    LinkedParameterHandle,
                    KnownParameters,
                    ENiagaraDefaultMode::FailIfPreviouslyNotSet,
                    FGuid());
#endif

                OutNodeId = OverridePin.PinId.ToString();
                if (OutLinkedParameter)
                {
                    *OutLinkedParameter = LinkedVariable.GetName().ToString();
                }
                if (OutLinkedParameterType)
                {
                    *OutLinkedParameterType = LinkType.GetName();
                }
                return FNiagaraEditError();
            }

            // Dynamic-input value mode: value = { dynamicInput: "<ScriptAssetPath>" } assigns a
            // dynamic-input function-call node onto a freshly created override pin (the write-side peer
            // of the linked-parameter path above). Like that path, the override pin carries the module
            // input's declared type and the assigned value's type is validated against it: the engine's
            // UNiagaraStackFunctionInput::SetDynamicInput types the pin from the module input's declared
            // type (InputType), so a type-incompatible dynamic input must be rejected, not silently
            // wired into a mistyped override.
            FString DynamicInputScriptPath;
            if (TryGetDynamicInputRequest(Payload.Value, DynamicInputScriptPath))
            {
                FNiagaraEditError LoadError;
                UNiagaraScript* DynamicInputScript = LoadDynamicInputScript(DynamicInputScriptPath, LoadError);
                if (!DynamicInputScript)
                {
                    return LoadError;
                }

                const FNiagaraTypeDefinition OutputType = GetDynamicInputOutputType(DynamicInputScript);
                if (!OutputType.IsValid())
                {
                    return FNiagaraEditError::Make(TEXT("DYNAMIC_INPUT_NO_OUTPUT_TYPE"),
                        FString::Printf(TEXT("Could not determine the value output type of dynamic-input script '%s'."), *DynamicInputScriptPath));
                }

                // Reject a dynamic input whose output type is incompatible with the module input's
                // declared type, mirroring the linked-parameter path's PARAMETER_TYPE_MISMATCH guard.
                // Skip the check only when the declared type could not be discovered (invalid) — the
                // engine schema still enforces type when the graph compiles in that case.
                if (DeclaredInputType.IsValid()
                    && !AreNiagaraParameterTypesCompatible(DeclaredInputType, OutputType))
                {
                    return FNiagaraEditError::Make(TEXT("PARAMETER_TYPE_MISMATCH"),
                        FString::Printf(
                            TEXT("Dynamic input '%s' outputs type '%s' but module input '%s' expects '%s'."),
                            *DynamicInputScriptPath,
                            *OutputType.GetName(),
                            *Payload.InputName,
                            *DeclaredInputType.GetName()));
                }

                // Type the override pin from the module input's declared type (what the engine's
                // GetOrCreateOverridePin uses), falling back to the dynamic input's own output type only
                // when the declared type is unknown. In the accepted (type-compatible) case these coincide.
                const FNiagaraTypeDefinition OverridePinType =
                    DeclaredInputType.IsValid() ? DeclaredInputType : OutputType;

                // Clear any prior override (literal default, dynamic-input chain, or stale link)
                // before creating the fresh override pin the dynamic-input node wires into — the
                // same precondition the linked-parameter path observes above, and ungated for the
                // same reason: assigning a dynamic input over an existing one is what the caller
                // means, so it proceeds. It still destroys the chain that was there — including a
                // nested one the response cannot reconstruct — so record what it displaced.
                ClearOverrideAndRecordReplacement(*Target.ModuleNode, AliasedInputHandle, *Target.Graph, InOutReplacedOverride);

                UEdGraphPin& OverridePin = FNiagaraStackGraphUtilities::GetOrCreateStackFunctionInputOverridePin(
                    *Target.ModuleNode,
                    AliasedInputHandle,
                    OverridePinType,
                    FGuid(),
                    FGuid());
                OverridePin.Modify();

                UNiagaraNodeFunctionCall* DynamicInputNode = nullptr;
                FNiagaraStackGraphUtilities::SetDynamicInputForFunctionInput(
                    OverridePin,
                    DynamicInputScript,
                    DynamicInputNode,
                    FGuid(),
                    FString(),
                    FGuid());
                if (!DynamicInputNode)
                {
                    return FNiagaraEditError::Make(TEXT("DYNAMIC_INPUT_SET_FAILED"),
                        TEXT("Failed to create the dynamic-input function-call node on the module input override pin."));
                }

                OutNodeId = OverridePin.PinId.ToString();
                if (OutDynamicInput)
                {
                    *OutDynamicInput = DynamicInputScriptPath;
                }
                return FNiagaraEditError();
            }

            FNiagaraTypeDefinition InputType;
            FString DefaultValue;
            FNiagaraVariable ExpectedTypedValue;
            UEdGraphPin* ExistingPin = nullptr;
            if (UEdGraphPin* ModuleInputPin = GetParameterMapPin(*Target.ModuleNode, EGPD_Input))
            {
                if (ModuleInputPin->LinkedTo.Num() == 1)
                {
                    if (UNiagaraNode* OverrideNode = Cast<UNiagaraNode>(ModuleInputPin->LinkedTo[0]->GetOwningNode()))
                    {
                        TArray<UEdGraphPin*> OverridePins;
                        OverrideNode->GetInputPins(OverridePins);
                        if (UEdGraphPin** ExistingPinPtr = OverridePins.FindByPredicate(
                            [&AliasedInputHandle](const UEdGraphPin* Pin)
                            {
                                return Pin && Pin->PinName == AliasedInputHandle.GetParameterHandleString();
                            }))
                        {
                            ExistingPin = *ExistingPinPtr;
                        }
                    }
                }
            }

            bool bTypedLiteralHandled = false;
            if (FNiagaraEditError EncodeError = EncodeTypedModuleInputLiteral(
                    Payload.InputName,
                    DeclaredInputType,
                    Payload.Value,
                    bTypedLiteralHandled,
                    DefaultValue,
                    ExpectedTypedValue);
                EncodeError.HasError())
            {
                return EncodeError;
            }

            if (bTypedLiteralHandled)
            {
                InputType = DeclaredInputType;
            }
            else if (ExistingPin)
            {
                InputType = UEdGraphSchema_Niagara::PinToTypeDefinition(ExistingPin);
                DefaultValue = JsonValueToPinDefaultString(Payload.Value);
            }
            else if (!InferNiagaraInputType(Payload.Value, InputType, DefaultValue))
            {
                return FNiagaraEditError::Make(TEXT("UNSUPPORTED_INPUT_VALUE"), TEXT("Module input values must be a bool, number, vector object/array, color object, or an existing override pin default string."));
            }

            if (DefaultValue.IsEmpty())
            {
                return FNiagaraEditError::Make(TEXT("INVALID_INPUT_VALUE"), FString::Printf(TEXT("Could not convert input '%s' value to a Niagara pin default string."), *Payload.InputName));
            }

            // An override pin that has an inbound link is not a value slot: the graph evaluates
            // the link and never reads the pin's DefaultValue. TrySetDefaultValue on such a pin
            // therefore lands a byte nothing consumes, and echoing it back reports a value the
            // graph is not using. Classify the pin with the same walk the inspect / asset.dump
            // readback uses, so what this branch refuses and what `valueMode` reports are one
            // decision rather than two that can drift.
            {
                TMap<FName, NiagaraEdit::FModuleInputBindingInfo> Bindings;
                NiagaraEdit::ClassifyModuleInputBindings(*Target.ModuleNode, Bindings);
                // Absent from the map means the input has no override pin at all (script
                // default); "local" means an override pin carrying a literal. Every other mode
                // — dynamicInput / linked / data / objectAsset / expression / connected — is an
                // inbound link a literal write cannot reach past.
                const NiagaraEdit::FModuleInputBindingInfo* Binding = Bindings.Find(AliasedInputHandle.GetName());
                if (Binding && Binding->ValueMode != TEXT("local"))
                {
                    const FString LinkSource = !Binding->LinkedParameter.IsEmpty()
                        ? Binding->LinkedParameter
                        : Binding->DynamicInputScript;
                    if (!InOutReplacedOverride || !InOutReplacedOverride->bBreakExistingLink)
                    {
                        return FNiagaraEditError::Make(TEXT("MODULE_INPUT_OVERRIDE_LINKED"),
                            FString::Printf(
                                TEXT("Module input '%s' is driven by an inbound link (valueMode '%s'%s%s), so a literal written on its override pin would never be read. Pass breakExistingLink:true to replace that link with this literal, or set the value on the driving source instead."),
                                *Payload.InputName,
                                *Binding->ValueMode,
                                LinkSource.IsEmpty() ? TEXT("") : TEXT(", source "),
                                *LinkSource));
                    }

                    // Opted in: tear the link and its now-orphaned upstream chain down before
                    // the write, the same clear the linked-parameter and dynamic-input branches
                    // above make, so the literal actually drives the input and the echo is true.
                    // This destroys ExistingPin — InputType was already copied off it above and
                    // the pointer must not be dereferenced past this point.
                    NiagaraResetModuleInput::ClearModuleInputOverride(*Target.ModuleNode, AliasedInputHandle, *Target.Graph);
                    InOutReplacedOverride->ValueMode = Binding->ValueMode;
                    InOutReplacedOverride->Source = LinkSource;
                }
            }

            UEdGraphPin& OverridePin = FNiagaraStackGraphUtilities::GetOrCreateStackFunctionInputOverridePin(
                *Target.ModuleNode,
                AliasedInputHandle,
                InputType,
                FGuid(),
                FGuid());
            OverridePin.Modify();
            GetDefault<UEdGraphSchema_Niagara>()->TrySetDefaultValue(OverridePin, DefaultValue, true);
            const FString ReadBackValue = OverridePin.GetDefaultAsString();
            const bool bPinTypeMatches = UEdGraphSchema_Niagara::PinToTypeDefinition(&OverridePin) == InputType;
            const bool bTypedValueMatches = !bTypedLiteralHandled || VerifyTypedModuleInputPin(OverridePin, ExpectedTypedValue);
            const bool bStringValueMatches = bTypedLiteralHandled || ReadBackValue == DefaultValue;
            if (!bPinTypeMatches || !bTypedValueMatches || !bStringValueMatches)
            {
                return FNiagaraEditError::Make(TEXT("INVALID_INPUT_VALUE"), FString::Printf(
                    TEXT("Module input '%s' failed override-pin read-back verification after writing declared type '%s'."),
                    *Payload.InputName,
                    *InputType.GetName()));
            }
            OutNodeId = OverridePin.PinId.ToString();
            if (OutWrittenValue)
            {
                // Echo the value read back from the override pin, not the requested encoding.
                *OutWrittenValue = ReadBackValue;
            }

            // The override pin is not the only place this input's value can live. For an input
            // whose type is rapid-iteration capable, a compile generates the constant
            // Constants.<Emitter>.<Module>.<Input> and the compiled simulation reads THAT, so a
            // pin written over a constant that already exists leaves two disagreeing sources and
            // the store is the one that governs. The Niagara editor never produces that state:
            // UNiagaraStackFunctionInput::SetLocalValue writes the constant on every affected
            // script. Push the same bytes through here so pin and store agree whichever one the
            // translator ends up reading, and so the value survives the next compile — the
            // emitter-stage stores are what PrepareRapidIterationParameters copies over the
            // system-script mirrors, so writing only the mirror (or only the pin) is discarded.
            if (OutRapidIteration)
            {
                const FNiagaraVariable PinVariable =
                    UEdGraphSchema_Niagara::PinToNiagaraVariable(&OverridePin, /*bNeedsValue=*/true);
                if (PinVariable.IsDataAllocated())
                {
                    PinWrightNiagara::WriteThroughModuleInputConstant(
                        Target.System,
                        Target.Emitter,
                        PinWrightNiagara::MakeRapidIterationConstantName(
                            AliasedInputHandle.GetParameterHandleString(),
                            Target.Emitter ? Target.Emitter->GetUniqueEmitterName() : FString(),
                            OutputNode->GetUsage()),
                        PinVariable.GetType(),
                        PinVariable.GetData(),
                        *OutRapidIteration);
                }
            }
            return FNiagaraEditError();
        }

        if (Operation == ENiagaraEditOperation::SetModuleScript)
        {
            UNiagaraScript* OldScript = Target.ModuleNode->FunctionScript;

            UNiagaraScript* NewScript = LoadObject<UNiagaraScript>(nullptr, *Payload.NewScriptPath);
            if (!NewScript)
            {
                return FNiagaraEditError::Make(TEXT("ASSET_NOT_FOUND"),
                    FString::Printf(TEXT("Could not load Niagara script '%s'."), *Payload.NewScriptPath));
            }

            // Usage compatibility: both scripts must have the same ENiagaraScriptUsage
            if (OldScript && OldScript->GetUsage() != NewScript->GetUsage())
            {
                return FNiagaraEditError::Make(TEXT("INCOMPATIBLE_SCRIPT_USAGE"),
                    FString::Printf(TEXT("New script usage does not match existing module usage.")));
            }

            // Stack-group compatibility via ModuleUsageBitmask
            const ENiagaraScriptUsage StackUsage = OutputNode->GetUsage();
            const FVersionedNiagaraScriptData* NewScriptData = NewScript->GetLatestScriptData();
            if (NewScriptData && !UNiagaraScript::IsSupportedUsageContextForBitmask(NewScriptData->ModuleUsageBitmask, StackUsage))
            {
                return FNiagaraEditError::Make(TEXT("INCOMPATIBLE_STACK_GROUP"),
                    FString::Printf(TEXT("New script's ModuleUsageBitmask does not support the current stack's script usage.")));
            }

            // Kill running instances before mutating to avoid rendering partial state
            if (Target.System)
            {
                // Accumulated so MakeMutationResult's `quiescedInstances` covers this
                // compile-independent kill too, not only the compile path's.
                Target.QuiescedInstances += PinWrightNiagara::KillSystemInstances(*Target.System);
            }

            // Snapshot existing overrides before swapping; exposed to caller via OutOverrideSnapshot
            // for response reporting so the handler body does not need a separate pre-transaction snapshot.
            TMap<FNiagaraVariable, FString> LocalSnapshot;
            TMap<FNiagaraVariable, FString>& OverrideSnapshot = OutOverrideSnapshot ? *OutOverrideSnapshot : LocalSnapshot;
            if (Payload.bPreserveOverrides)
            {
                NiagaraEdit::SnapshotInputOverrides(*Target.ModuleNode, OverrideSnapshot);
            }

            // Swap the script
            Target.ModuleNode->Modify();
            Target.ModuleNode->FunctionScript = NewScript;
            Target.ModuleNode->SelectedScriptVersion = Payload.NewScriptVersion.IsValid()
                ? Payload.NewScriptVersion
                : NewScript->GetExposedVersion().VersionGuid;
            // InvalidScriptVersionReference is protected; clear it via FProperty reflection so a stale
            // reference from a prior swap does not produce a spurious warning after the new script binds.
            if (FProperty* InvalidVersionProp = UNiagaraNodeFunctionCall::StaticClass()->FindPropertyByName(TEXT("InvalidScriptVersionReference")))
            {
                if (FGuid* InvalidVersionPtr = InvalidVersionProp->ContainerPtrToValuePtr<FGuid>(Target.ModuleNode))
                {
                    *InvalidVersionPtr = FGuid();
                }
            }
            Target.ModuleNode->MarkNodeRequiresSynchronization(__FUNCTION__, true);
            Target.ModuleNode->RefreshFromExternalChanges();

            // Enumerate new script's exposed inputs
            TArray<FNiagaraVariable> NewInputs;
            NiagaraEdit::EnumerateScriptInputs(*NewScript, NewInputs);

            // Re-apply preserved overrides and track dropped / added
            OutNodeId = Target.ModuleNode->NodeGuid.ToString();

            if (Payload.bPreserveOverrides && !OverrideSnapshot.IsEmpty())
            {
                for (const TTuple<FNiagaraVariable, FString>& Entry : OverrideSnapshot)
                {
                    const FNiagaraVariable& OldVar = Entry.Key;
                    const FString& OldValue = Entry.Value;

                    // Check if new script has an input with the same name
                    const FNiagaraVariable* MatchingVar = NewInputs.FindByPredicate(
                        [&OldVar](const FNiagaraVariable& Candidate)
                        {
                            return Candidate.GetName() == OldVar.GetName();
                        });

                    if (!MatchingVar)
                    {
                        // Input removed: skip (will be reported by caller via droppedOverrides)
                        continue;
                    }
                    if (MatchingVar->GetType() != OldVar.GetType())
                    {
                        // Type changed: skip
                        continue;
                    }

                    // Re-apply: reconstruct aliased handle and write the override pin
                    const FNiagaraParameterHandle InputHandle = FNiagaraParameterHandle::CreateModuleParameterHandle(OldVar.GetName());
                    const FNiagaraParameterHandle AliasedInputHandle = FNiagaraParameterHandle::CreateAliasedModuleParameterHandle(InputHandle, Target.ModuleNode);

                    UEdGraphPin& OverridePin = FNiagaraStackGraphUtilities::GetOrCreateStackFunctionInputOverridePin(
                        *Target.ModuleNode,
                        AliasedInputHandle,
                        OldVar.GetType(),
                        FGuid(),
                        FGuid());
                    OverridePin.Modify();
                    GetDefault<UEdGraphSchema_Niagara>()->TrySetDefaultValue(OverridePin, OldValue, true);
                }
            }

            return FNiagaraEditError();
        }

        if (Operation == ENiagaraEditOperation::RemoveModule)
        {
            FLocalStackNodeGroup SourceGroup = Groups[SourceGroupIndex];
            if (!DisconnectStackGroup(SourceGroup, Groups[SourceGroupIndex - 1], Groups[SourceGroupIndex + 1]))
            {
                return FNiagaraEditError::Make(TEXT("MODULE_REMOVE_FAILED"), TEXT("Failed to reconnect the stack around the removed module."));
            }

            TArray<UNiagaraNode*> NodesToRemove;
            GetGroupNodesRecursive(SourceGroup.StartNodes, SourceGroup.EndNode, NodesToRemove);
            for (UNiagaraNode* NodeToRemove : NodesToRemove)
            {
                if (NodeToRemove)
                {
                    NodeToRemove->Modify();
                    Target.Graph->RemoveNode(NodeToRemove);
                }
            }
            return FNiagaraEditError();
        }

        if (Operation == ENiagaraEditOperation::MoveModule)
        {
            const int32 ModuleCount = Groups.Num() - 2;
            if (Payload.Target.ToIndex < 0 || Payload.Target.ToIndex >= ModuleCount)
            {
                return FNiagaraEditError::Make(TEXT("MODULE_INDEX_INVALID"), FString::Printf(TEXT("Module toIndex %d is out of range."), Payload.Target.ToIndex));
            }
            if (Payload.Target.ToIndex == SourceGroupIndex - 1)
            {
                OutIndex = Payload.Target.ToIndex;
                return FNiagaraEditError();
            }

            FLocalStackNodeGroup SourceGroup = Groups[SourceGroupIndex];
            if (!DisconnectStackGroup(SourceGroup, Groups[SourceGroupIndex - 1], Groups[SourceGroupIndex + 1]))
            {
                return FNiagaraEditError::Make(TEXT("MODULE_MOVE_FAILED"), TEXT("Failed to disconnect the module from its current stack position."));
            }

            TArray<FLocalStackNodeGroup> RebuiltGroups;
            GetStackNodeGroups(*OutputNode, RebuiltGroups);
            const int32 InsertGroupIndex = FMath::Clamp(Payload.Target.ToIndex + 1, 1, RebuiltGroups.Num() - 1);
            if (!RebuiltGroups.IsValidIndex(InsertGroupIndex) || !RebuiltGroups.IsValidIndex(InsertGroupIndex - 1))
            {
                return FNiagaraEditError::Make(TEXT("MODULE_MOVE_FAILED"), TEXT("Failed to resolve destination stack position."));
            }
            if (!ConnectStackGroup(SourceGroup, RebuiltGroups[InsertGroupIndex - 1], RebuiltGroups[InsertGroupIndex]))
            {
                return FNiagaraEditError::Make(TEXT("MODULE_MOVE_FAILED"), TEXT("Failed to reconnect the module at the destination stack position."));
            }

            // Chain reconnect leaves NodePosY stale; rewrite it so the NodePosY-sorted readbacks
            // reflect the move (see RelayoutModuleNodePositions for the full rationale).
            RelayoutModuleNodePositions(*OutputNode);

            OutIndex = Payload.Target.ToIndex;
            return FNiagaraEditError();
        }

        return FNiagaraEditError::Make(TEXT("INVALID_OPERATION"), TEXT("Unsupported module mutation."));
    }

    FNiagaraEditError ApplyPinMutation(
        const FNiagaraPinEditPayload& Payload,
        ENiagaraEditOperation Operation,
        FNiagaraResolvedTarget& Target,
        FString& OutDefaultValue)
    {
        if (!Target.Graph)
        {
            return FNiagaraEditError::Make(TEXT("GRAPH_NOT_FOUND"), TEXT("Graph target was not resolved."));
        }

        const UEdGraphSchema* Schema = Target.Graph->GetSchema();
        if (!Schema)
        {
            return FNiagaraEditError::Make(TEXT("SCHEMA_NOT_FOUND"), TEXT("Niagara graph schema is not available."));
        }

        if (Operation == ENiagaraEditOperation::SetPinDefault)
        {
            UEdGraphNode* Node = FindGraphNodeById(Target.Graph, Payload.NodeId);
            UEdGraphPin* Pin = FindGraphPinByName(Node, Payload.PinName);
            if (!Node || !Pin)
            {
                return FNiagaraEditError::Make(TEXT("PIN_NOT_FOUND"), FString::Printf(TEXT("Pin '%s' on node '%s' was not found."), *Payload.PinName, *Payload.NodeId));
            }
            OutDefaultValue = JsonValueToPinDefaultString(Payload.DefaultValue);
            if (OutDefaultValue.IsEmpty())
            {
                return FNiagaraEditError::Make(TEXT("INVALID_PIN_DEFAULT"), TEXT("Pin default must be a string, bool, number, vector array/object, or color object."));
            }
            Pin->Modify();
            Schema->TrySetDefaultValue(*Pin, OutDefaultValue, true);
            Target.Node = Node;
            Target.Pin = Pin;
            return FNiagaraEditError();
        }

        UEdGraphNode* FromNode = FindGraphNodeById(Target.Graph, Payload.FromNode);
        UEdGraphNode* ToNode = FindGraphNodeById(Target.Graph, Payload.ToNode);
        UEdGraphPin* FromPin = FindGraphPinByName(FromNode, Payload.FromPin);
        UEdGraphPin* ToPin = FindGraphPinByName(ToNode, Payload.ToPin);
        if (!FromNode || !ToNode || !FromPin || !ToPin)
        {
            return FNiagaraEditError::Make(TEXT("PIN_NOT_FOUND"), TEXT("Source or destination node/pin was not found."));
        }

        if (Operation == ENiagaraEditOperation::ConnectPin)
        {
            const FPinConnectionResponse Response = Schema->CanCreateConnection(FromPin, ToPin);
            if (Response.Response == CONNECT_RESPONSE_DISALLOW)
            {
                return FNiagaraEditError::Make(TEXT("CONNECTION_DISALLOWED"), Response.Message.ToString());
            }

            FromPin->Modify();
            ToPin->Modify();
            if (!Schema->TryCreateConnection(FromPin, ToPin))
            {
                return FNiagaraEditError::Make(TEXT("CONNECTION_FAILED"), TEXT("Niagara graph schema rejected the pin connection."));
            }
            return FNiagaraEditError();
        }

        if (!FromPin->LinkedTo.Contains(ToPin))
        {
            return FNiagaraEditError::Make(TEXT("LINK_NOT_FOUND"), TEXT("Requested pin link does not exist."));
        }

        FromPin->Modify();
        ToPin->Modify();
        Schema->BreakSinglePinLink(FromPin, ToPin);
        return FNiagaraEditError();
    }

    double GetArrayNumber(const TSharedPtr<FJsonValue>& Value, int32 Index, double DefaultValue = 0.0)
    {
        if (!Value.IsValid() || Value->Type != EJson::Array || !Value->AsArray().IsValidIndex(Index))
        {
            return DefaultValue;
        }
        const TSharedPtr<FJsonValue>& Element = Value->AsArray()[Index];
        return Element.IsValid() && Element->Type == EJson::Number ? Element->AsNumber() : DefaultValue;
    }

    double GetObjectNumber(const TSharedPtr<FJsonValue>& Value, const TCHAR* Field, double DefaultValue = 0.0)
    {
        if (!Value.IsValid() || Value->Type != EJson::Object)
        {
            return DefaultValue;
        }
        double Number = DefaultValue;
        Value->AsObject()->TryGetNumberField(Field, Number);
        return Number;
    }

    FVector3f ParseVector3fValue(const TSharedPtr<FJsonValue>& Value)
    {
        if (Value.IsValid() && Value->Type == EJson::Array)
        {
            return FVector3f(
                static_cast<float>(GetArrayNumber(Value, 0)),
                static_cast<float>(GetArrayNumber(Value, 1)),
                static_cast<float>(GetArrayNumber(Value, 2)));
        }
        return FVector3f(
            static_cast<float>(GetObjectNumber(Value, TEXT("x"))),
            static_cast<float>(GetObjectNumber(Value, TEXT("y"))),
            static_cast<float>(GetObjectNumber(Value, TEXT("z"))));
    }

    FVector ParseVectorValue(const TSharedPtr<FJsonValue>& Value)
    {
        const FVector3f Vector = ParseVector3fValue(Value);
        return FVector(Vector.X, Vector.Y, Vector.Z);
    }

    FLinearColor ParseColorValue(const TSharedPtr<FJsonValue>& Value)
    {
        if (Value.IsValid() && Value->Type == EJson::Array)
        {
            return FLinearColor(
                static_cast<float>(GetArrayNumber(Value, 0)),
                static_cast<float>(GetArrayNumber(Value, 1)),
                static_cast<float>(GetArrayNumber(Value, 2)),
                static_cast<float>(GetArrayNumber(Value, 3, 1.0)));
        }
        return FLinearColor(
            static_cast<float>(GetObjectNumber(Value, TEXT("r"))),
            static_cast<float>(GetObjectNumber(Value, TEXT("g"))),
            static_cast<float>(GetObjectNumber(Value, TEXT("b"))),
            static_cast<float>(GetObjectNumber(Value, TEXT("a"), 1.0)));
    }

    const FNiagaraVariable* FindParameterByName(const FNiagaraParameterStore& Store, const FString& Name, TArray<FNiagaraVariable>& Scratch)
    {
        Scratch.Reset();
        Store.GetParameters(Scratch);
        for (const FNiagaraVariable& Variable : Scratch)
        {
            if (Variable.GetName().ToString().Equals(Name, ESearchCase::IgnoreCase))
            {
                return &Variable;
            }
        }
        return nullptr;
    }

    bool AreNiagaraParameterTypesCompatible(const FNiagaraTypeDefinition& ExistingType, const FNiagaraTypeDefinition& RequestedType)
    {
        return ExistingType == RequestedType || ExistingType.IsSameBaseDefinition(RequestedType);
    }

    // A set_module_input `value` of the form { "link": "User.WispColor" } (or the
    // alias "parameter") requests a linked-parameter override rather than a literal.
    // Returns true and fills OutParameterName when the value carries a non-empty link
    // string. A plain string / number / object value is NOT a link (that stays literal),
    // so the only link spelling is the explicit {link|parameter: "<Namespace.Name>"} object.
    bool TryGetLinkedParameterRequest(const TSharedPtr<FJsonValue>& Value, FString& OutParameterName)
    {
        if (!Value.IsValid() || Value->Type != EJson::Object)
        {
            return false;
        }
        const TSharedPtr<FJsonObject> Object = Value->AsObject();
        if (!Object.IsValid())
        {
            return false;
        }
        FString LinkName;
        if (Object->TryGetStringField(TEXT("link"), LinkName)
            || Object->TryGetStringField(TEXT("parameter"), LinkName))
        {
            LinkName.TrimStartAndEndInline();
            if (!LinkName.IsEmpty())
            {
                OutParameterName = LinkName;
                return true;
            }
        }
        return false;
    }

    // Dynamic-input value mode detector: value = { dynamicInput: "<ScriptAssetPath>" } requests that the
    // module input be driven by a dynamic-input script node rather than a literal or a linked parameter.
    // Parallel to TryGetLinkedParameterRequest. (Recursive nested-input authoring — { dynamicInput,
    // inputs } — is tracked in F-niagara-dynamic-input-nested-inputs; it needs resolver-based stack-input
    // enumeration that EnumerateScriptInputs, which only surfaces the parameter-map pin, cannot provide.)
    bool TryGetDynamicInputRequest(const TSharedPtr<FJsonValue>& Value, FString& OutScriptPath)
    {
        if (!Value.IsValid() || Value->Type != EJson::Object)
        {
            return false;
        }
        const TSharedPtr<FJsonObject> Object = Value->AsObject();
        if (!Object.IsValid())
        {
            return false;
        }
        FString ScriptPath;
        if (!Object->TryGetStringField(TEXT("dynamicInput"), ScriptPath))
        {
            return false;
        }
        ScriptPath.TrimStartAndEndInline();
        if (ScriptPath.IsEmpty())
        {
            return false;
        }
        OutScriptPath = ScriptPath;
        return true;
    }

    // Load a dynamic-input script by path and confirm its usage is DynamicInput. Returns null and fills
    // OutError on a missing asset or a non-dynamic-input script (a module/particle script wired here
    // would otherwise produce a malformed override chain).
    UNiagaraScript* LoadDynamicInputScript(const FString& ScriptPath, FNiagaraEditError& OutError)
    {
        UNiagaraScript* Script = LoadObject<UNiagaraScript>(nullptr, *ScriptPath);
        if (!Script)
        {
            OutError = FNiagaraEditError::Make(TEXT("ASSET_NOT_FOUND"),
                FString::Printf(TEXT("Could not load dynamic-input Niagara script '%s'."), *ScriptPath));
            return nullptr;
        }
        if (Script->GetUsage() != ENiagaraScriptUsage::DynamicInput)
        {
            OutError = FNiagaraEditError::Make(TEXT("NOT_DYNAMIC_INPUT_SCRIPT"),
                FString::Printf(TEXT("Niagara script '%s' is not a DynamicInput-usage script; a { dynamicInput } value requires one."), *ScriptPath));
            return nullptr;
        }
        return Script;
    }

    // The dynamic-input script's own value output type (Add_Float -> float, etc.). The caller validates
    // this against the module input's declared type before assigning, so a type-incompatible dynamic
    // input is rejected rather than wired into a mistyped override pin.
    // EnumerateScriptInputs cannot supply this (it only surfaces the parameter-map input), so read the
    // DynamicInput-usage output node directly, mirroring NiagaraModelBuilder's
    // GetNodesOfClass<UNiagaraNodeOutput> pattern (UNiagaraGraph::FindOutputNode is not exported).
    FNiagaraTypeDefinition GetDynamicInputOutputType(UNiagaraScript* Script)
    {
        if (!Script)
        {
            return FNiagaraTypeDefinition();
        }
        UNiagaraScriptSource* Source = Cast<UNiagaraScriptSource>(Script->GetLatestSource());
        if (!Source || !Source->NodeGraph)
        {
            return FNiagaraTypeDefinition();
        }
        TArray<UNiagaraNodeOutput*> OutputNodes;
        Source->NodeGraph->GetNodesOfClass<UNiagaraNodeOutput>(OutputNodes);
        for (const UNiagaraNodeOutput* OutputNode : OutputNodes)
        {
            if (!OutputNode || OutputNode->GetUsage() != ENiagaraScriptUsage::DynamicInput)
            {
                continue;
            }
            for (const FNiagaraVariable& Output : OutputNode->GetOutputs())
            {
                const FNiagaraTypeDefinition& OutputType = Output.GetType();
                if (OutputType.IsValid() && OutputType != FNiagaraTypeDefinition::GetParameterMapDef())
                {
                    return OutputType;
                }
            }
        }
        return FNiagaraTypeDefinition();
    }

    // True when the parameter name lives in a namespace whose values are supplied by the
    // engine / simulation context rather than the system's editable user store (Engine.*,
    // System.*, Emitter.*, Particles.*, Module.*, etc.). Such parameters can be linked
    // without an existence/type pre-check because they are not enumerable from the user
    // store; User.* parameters, in contrast, must already exist (mirrors the
    // B-set-niagara-param-no-validation lesson — no silent links to a missing user knob).
    bool IsUserScopeParameterName(const FString& ParameterName)
    {
        return ParameterName.StartsWith(TEXT("User."), ESearchCase::IgnoreCase);
    }

    // Resolve a linked-parameter request against the system's exposed (User) parameter store.
    // On success OutVariable carries the fully-qualified name (User.Name) and the parameter's
    // declared type. Returns an error when a User.* parameter does not exist. For non-User
    // namespaces, the type is taken from the module input's declared type (InputType) since
    // engine/system-scope values are not stored in the user store.
    FNiagaraEditError ResolveLinkedParameter(
        UNiagaraSystem* System,
        const FString& ParameterName,
        const FNiagaraTypeDefinition& InputType,
        FNiagaraVariable& OutVariable)
    {
        if (IsUserScopeParameterName(ParameterName))
        {
            if (!System)
            {
                return FNiagaraEditError::Make(TEXT("PARAMETER_NOT_FOUND"),
                    FString::Printf(TEXT("Cannot resolve user parameter '%s' — the target has no owning Niagara System."), *ParameterName));
            }
            // Resolve through the redirection store's own override: FindParameterVariable
            // with IgnoreType=true resolves the "User." prefix (so the name matches whether
            // passed with or without the namespace) and returns the stored entry carrying the
            // parameter's real name + type — the same idiom as NiagaraGraphHandler.cpp:498 /
            // NiagaraInstanceUtils.cpp:73.
            const FNiagaraUserRedirectionParameterStore& UserStore = System->GetExposedParameters();
            const FNiagaraVariableWithOffset* Existing =
                UserStore.FindParameterVariable(FNiagaraVariable(InputType, FName(*ParameterName)), /*IgnoreType=*/true);
            if (Existing)
            {
                OutVariable = FNiagaraVariable(Existing->GetType(), Existing->GetName());
                return FNiagaraEditError();
            }
            return FNiagaraEditError::Make(TEXT("PARAMETER_NOT_FOUND"),
                FString::Printf(TEXT("User parameter '%s' does not exist; create it with niagara.add_parameter before linking."), *ParameterName));
        }

        // Engine/system/attribute scope: not enumerable from the user store. Type-match the
        // module input so the link reads a value of the right shape; existence is the engine's
        // responsibility at compile time.
        if (!InputType.IsValid())
        {
            return FNiagaraEditError::Make(TEXT("PARAMETER_NOT_FOUND"),
                FString::Printf(TEXT("Cannot infer a type for linked parameter '%s' (module input type unknown)."), *ParameterName));
        }
        OutVariable = FNiagaraVariable(InputType, FName(*ParameterName));
        return FNiagaraEditError();
    }

    // Enumerate the same placed stack inputs used by the schema/readback paths. The utility returns
    // names carrying the Module. namespace; inputName is the bare top-level spelling on the wire.
    bool TryFindModuleStackInput(
        const UNiagaraNodeFunctionCall* ModuleNode,
        const FString& InputName,
        FNiagaraTypeDefinition* OutType,
        TArray<FString>* OutAvailableNames)
    {
        if (OutType)
        {
            *OutType = FNiagaraTypeDefinition();
        }
        if (OutAvailableNames)
        {
            OutAvailableNames->Reset();
        }
        if (!ModuleNode)
        {
            return false;
        }

        TArray<FNiagaraVariable> Inputs;
        NiagaraEdit::EnumerateModuleStackInputs(*ModuleNode, Inputs);
        bool bFound = false;
        for (const FNiagaraVariable& Input : Inputs)
        {
            const FString ShortInputName = FNiagaraParameterHandle(Input.GetName()).GetName().ToString();
            if (OutAvailableNames && !ShortInputName.IsEmpty())
            {
                OutAvailableNames->AddUnique(ShortInputName);
            }
            if (!bFound && ShortInputName.Equals(InputName, ESearchCase::IgnoreCase))
            {
                if (OutType)
                {
                    *OutType = Input.GetType();
                }
                bFound = true;
            }
        }
        return bFound;
    }

    FNiagaraEditError SetScriptStructParameterValue(
        FNiagaraParameterStore& Store,
        const FNiagaraVariable& Variable,
        const FString& Type,
        const TSharedPtr<FJsonValue>& Value,
        bool bAdd)
    {
        UScriptStruct* Struct = Variable.GetType().GetScriptStruct();
        if (!Struct || !Value.IsValid() || Value->Type != EJson::Object)
        {
            return FNiagaraEditError::Make(TEXT("INVALID_VALUE"), FString::Printf(TEXT("Parameter type '%s' requires a JSON object value."), *Type));
        }

        TSharedPtr<FJsonObject> ValueObject = Value->AsObject();
        const TSharedPtr<FJsonObject>* FieldsObject = nullptr;
        if (!ValueObject->TryGetObjectField(TEXT("fields"), FieldsObject) || !FieldsObject || !FieldsObject->IsValid())
        {
            FieldsObject = &ValueObject;
        }

        FStructOnScope StructScope(Struct);
        uint8* StructData = StructScope.GetStructMemory();
        if (!StructData)
        {
            return FNiagaraEditError::Make(TEXT("INVALID_VALUE"), FString::Printf(TEXT("Failed to allocate storage for parameter type '%s'."), *Type));
        }

        for (const TPair<FString, TSharedPtr<FJsonValue>> Pair : (*FieldsObject)->Values)
        {
            if (Pair.Key.Equals(TEXT("_kind"), ESearchCase::IgnoreCase)
                || Pair.Key.Equals(TEXT("scriptStruct"), ESearchCase::IgnoreCase))
            {
                continue;
            }

            FProperty* Property = FindPropertyCI(Struct, Pair.Key);
            if (!Property)
            {
                return FNiagaraEditError::Make(
                    TEXT("INVALID_VALUE"),
                    FString::Printf(TEXT("Parameter type '%s' has no field '%s'."), *Type, *Pair.Key));
            }

            FString ApplyError;
            if (!ApplyJsonValueToProperty(StructData, Property, Pair.Value, ApplyError))
            {
                return FNiagaraEditError::Make(
                    TEXT("INVALID_VALUE"),
                    FString::Printf(TEXT("Failed to apply field '%s' for parameter type '%s': %s"), *Pair.Key, *Type, *ApplyError));
            }
        }

        return Store.SetParameterData(StructData, Variable, bAdd)
            ? FNiagaraEditError()
            : FNiagaraEditError::Make(TEXT("PARAMETER_SET_FAILED"), FString::Printf(TEXT("Failed to set struct parameter '%s'."), *Variable.GetName().ToString()));
    }

    // `Type` is the caller's spelling and is used only in messages; the write itself is dispatched
    // on ResolvedType, which NiagaraEdit::ValidateTypedValue resolved from the same table when it
    // decided which value shape to demand. Re-parsing the string here is what let the two disagree.
    FNiagaraEditError SetTypedParameterValue(
        FNiagaraParameterStore& Store,
        const FNiagaraVariable& Variable,
        const FString& Type,
        const PinWrightNiagara::FNiagaraResolvedParameterType& ResolvedType,
        const TSharedPtr<FJsonValue>& Value,
        bool bAdd)
    {
        using EValueKind = PinWrightNiagara::ENiagaraParameterValueKind;
        switch (ResolvedType.Kind)
        {
        case EValueKind::Float:
        {
            const float TypedValue = static_cast<float>(Value->AsNumber());
            return Store.SetParameterValue(TypedValue, Variable, bAdd)
                ? FNiagaraEditError()
                : FNiagaraEditError::Make(TEXT("PARAMETER_SET_FAILED"), FString::Printf(TEXT("Failed to set float parameter '%s'."), *Variable.GetName().ToString()));
        }
        case EValueKind::Int:
        {
            const int32 TypedValue = static_cast<int32>(Value->AsNumber());
            return Store.SetParameterValue(TypedValue, Variable, bAdd)
                ? FNiagaraEditError()
                : FNiagaraEditError::Make(TEXT("PARAMETER_SET_FAILED"), FString::Printf(TEXT("Failed to set int32 parameter '%s'."), *Variable.GetName().ToString()));
        }
        case EValueKind::Bool:
        {
            const FNiagaraBool TypedValue(Value->AsBool());
            return Store.SetParameterValue(TypedValue, Variable, bAdd)
                ? FNiagaraEditError()
                : FNiagaraEditError::Make(TEXT("PARAMETER_SET_FAILED"), FString::Printf(TEXT("Failed to set bool parameter '%s'."), *Variable.GetName().ToString()));
        }
        case EValueKind::Vec3:
        {
            const FVector3f TypedValue = ParseVector3fValue(Value);
            return Store.SetParameterValue(TypedValue, Variable, bAdd)
                ? FNiagaraEditError()
                : FNiagaraEditError::Make(TEXT("PARAMETER_SET_FAILED"), FString::Printf(TEXT("Failed to set vector parameter '%s'."), *Variable.GetName().ToString()));
        }
        case EValueKind::Position:
        {
            return Store.SetPositionParameterValue(ParseVectorValue(Value), Variable.GetName(), bAdd)
                ? FNiagaraEditError()
                : FNiagaraEditError::Make(TEXT("PARAMETER_SET_FAILED"), FString::Printf(TEXT("Failed to set position parameter '%s'."), *Variable.GetName().ToString()));
        }
        case EValueKind::LinearColor:
        {
            const FLinearColor TypedValue = ParseColorValue(Value);
            return Store.SetParameterValue(TypedValue, Variable, bAdd)
                ? FNiagaraEditError()
                : FNiagaraEditError::Make(TEXT("PARAMETER_SET_FAILED"), FString::Printf(TEXT("Failed to set color parameter '%s'."), *Variable.GetName().ToString()));
        }
        case EValueKind::Vec2:
        {
            FVector2f TypedValue(0.0f, 0.0f);
            if (Value.IsValid() && Value->Type == EJson::Array)
            {
                TypedValue = FVector2f(
                    static_cast<float>(GetArrayNumber(Value, 0)),
                    static_cast<float>(GetArrayNumber(Value, 1)));
            }
            else
            {
                TypedValue = FVector2f(
                    static_cast<float>(GetObjectNumber(Value, TEXT("x"))),
                    static_cast<float>(GetObjectNumber(Value, TEXT("y"))));
            }
            return Store.SetParameterValue(TypedValue, Variable, bAdd)
                ? FNiagaraEditError()
                : FNiagaraEditError::Make(TEXT("PARAMETER_SET_FAILED"), FString::Printf(TEXT("Failed to set vec2 parameter '%s'."), *Variable.GetName().ToString()));
        }
        case EValueKind::NiagaraID:
        {
            FNiagaraID TypedValue;
            if (Value.IsValid() && Value->Type == EJson::Array)
            {
                TypedValue.Index = static_cast<int32>(GetArrayNumber(Value, 0));
                TypedValue.AcquireTag = static_cast<int32>(GetArrayNumber(Value, 1));
            }
            else
            {
                TypedValue.Index = static_cast<int32>(GetObjectNumber(Value, TEXT("index")));
                TypedValue.AcquireTag = static_cast<int32>(GetObjectNumber(Value, TEXT("acquireTag")));
            }
            return Store.SetParameterValue(TypedValue, Variable, bAdd)
                ? FNiagaraEditError()
                : FNiagaraEditError::Make(TEXT("PARAMETER_SET_FAILED"), FString::Printf(TEXT("Failed to set NiagaraID parameter '%s'."), *Variable.GetName().ToString()));
        }
        case EValueKind::NiagaraSpawnInfo:
        {
            FNiagaraSpawnInfo TypedValue;
            TypedValue.Count = static_cast<int32>(GetObjectNumber(Value, TEXT("count")));
            TypedValue.InterpStartDt = static_cast<float>(GetObjectNumber(Value, TEXT("interpStartDt")));
            TypedValue.IntervalDt = static_cast<float>(GetObjectNumber(Value, TEXT("intervalDt"), 1.0));
            TypedValue.SpawnGroup = static_cast<int32>(GetObjectNumber(Value, TEXT("spawnGroup")));
            return Store.SetParameterValue(TypedValue, Variable, bAdd)
                ? FNiagaraEditError()
                : FNiagaraEditError::Make(TEXT("PARAMETER_SET_FAILED"), FString::Printf(TEXT("Failed to set NiagaraSpawnInfo parameter '%s'."), *Variable.GetName().ToString()));
        }
        case EValueKind::Half:
        {
            FNiagaraHalf TypedValue;
            FFloat16 Encoded;
            Encoded.Set(static_cast<float>(Value->AsNumber()));
            TypedValue.Value = Encoded.Encoded;
            return Store.SetParameterValue(TypedValue, Variable, bAdd)
                ? FNiagaraEditError()
                : FNiagaraEditError::Make(TEXT("PARAMETER_SET_FAILED"), FString::Printf(TEXT("Failed to set half parameter '%s'."), *Variable.GetName().ToString()));
        }
        case EValueKind::HalfVec2:
        {
            const bool bArray = Value.IsValid() && Value->Type == EJson::Array;
            const double XSrc = bArray ? GetArrayNumber(Value, 0) : GetObjectNumber(Value, TEXT("x"));
            const double YSrc = bArray ? GetArrayNumber(Value, 1) : GetObjectNumber(Value, TEXT("y"));
            FFloat16 X; X.Set(static_cast<float>(XSrc));
            FFloat16 Y; Y.Set(static_cast<float>(YSrc));
            FNiagaraHalfVector2 TypedValue;
            TypedValue.x = X.Encoded;
            TypedValue.y = Y.Encoded;
            return Store.SetParameterValue(TypedValue, Variable, bAdd)
                ? FNiagaraEditError()
                : FNiagaraEditError::Make(TEXT("PARAMETER_SET_FAILED"), FString::Printf(TEXT("Failed to set half2 parameter '%s'."), *Variable.GetName().ToString()));
        }
        case EValueKind::HalfVec3:
        {
            const bool bArray = Value.IsValid() && Value->Type == EJson::Array;
            const double XSrc = bArray ? GetArrayNumber(Value, 0) : GetObjectNumber(Value, TEXT("x"));
            const double YSrc = bArray ? GetArrayNumber(Value, 1) : GetObjectNumber(Value, TEXT("y"));
            const double ZSrc = bArray ? GetArrayNumber(Value, 2) : GetObjectNumber(Value, TEXT("z"));
            FFloat16 X; X.Set(static_cast<float>(XSrc));
            FFloat16 Y; Y.Set(static_cast<float>(YSrc));
            FFloat16 Z; Z.Set(static_cast<float>(ZSrc));
            FNiagaraHalfVector3 TypedValue;
            TypedValue.x = X.Encoded;
            TypedValue.y = Y.Encoded;
            TypedValue.z = Z.Encoded;
            return Store.SetParameterValue(TypedValue, Variable, bAdd)
                ? FNiagaraEditError()
                : FNiagaraEditError::Make(TEXT("PARAMETER_SET_FAILED"), FString::Printf(TEXT("Failed to set half3 parameter '%s'."), *Variable.GetName().ToString()));
        }
        case EValueKind::HalfVec4:
        {
            const bool bArray = Value.IsValid() && Value->Type == EJson::Array;
            const double XSrc = bArray ? GetArrayNumber(Value, 0) : GetObjectNumber(Value, TEXT("x"));
            const double YSrc = bArray ? GetArrayNumber(Value, 1) : GetObjectNumber(Value, TEXT("y"));
            const double ZSrc = bArray ? GetArrayNumber(Value, 2) : GetObjectNumber(Value, TEXT("z"));
            const double WSrc = bArray ? GetArrayNumber(Value, 3) : GetObjectNumber(Value, TEXT("w"));
            FFloat16 X; X.Set(static_cast<float>(XSrc));
            FFloat16 Y; Y.Set(static_cast<float>(YSrc));
            FFloat16 Z; Z.Set(static_cast<float>(ZSrc));
            FFloat16 W; W.Set(static_cast<float>(WSrc));
            FNiagaraHalfVector4 TypedValue;
            TypedValue.x = X.Encoded;
            TypedValue.y = Y.Encoded;
            TypedValue.z = Z.Encoded;
            TypedValue.w = W.Encoded;
            return Store.SetParameterValue(TypedValue, Variable, bAdd)
                ? FNiagaraEditError()
                : FNiagaraEditError::Make(TEXT("PARAMETER_SET_FAILED"), FString::Printf(TEXT("Failed to set half4 parameter '%s'."), *Variable.GetName().ToString()));
        }
        case EValueKind::ScriptStruct:
        default:
        {
            if (AreNiagaraParameterTypesCompatible(Variable.GetType(), ResolvedType.Definition))
            {
                return SetScriptStructParameterValue(Store, Variable, Type, Value, bAdd);
            }
            return FNiagaraEditError::Make(
                TEXT("INVALID_PARAMETER_TYPE"),
                FString::Printf(TEXT("Niagara parameter type '%s' is not supported by the safe parameter-store edit path."), *Type));
        }
        }
    }

    FNiagaraEditError ApplyParameterMutation(
        const FNiagaraParameterEditPayload& Payload,
        ENiagaraEditOperation Operation,
        FNiagaraResolvedTarget& Target)
    {
        if (!Target.ParameterStore)
        {
            return FNiagaraEditError::Make(TEXT("PARAMETER_STORE_NOT_FOUND"), TEXT("Resolved target has no parameter store."));
        }

        FNiagaraParameterStore& Store = *Target.ParameterStore;
        TArray<FNiagaraVariable> ExistingParameters;
        const FNiagaraVariable* ExistingByName = FindParameterByName(Store, Payload.Name, ExistingParameters);

        if (Operation == ENiagaraEditOperation::RemoveParameter)
        {
            if (!ExistingByName)
            {
                return FNiagaraEditError::Make(TEXT("PARAMETER_NOT_FOUND"), FString::Printf(TEXT("Parameter '%s' was not found."), *Payload.Name));
            }
            return Store.RemoveParameter(*ExistingByName)
                ? FNiagaraEditError()
                : FNiagaraEditError::Make(TEXT("PARAMETER_REMOVE_FAILED"), FString::Printf(TEXT("Failed to remove parameter '%s'."), *Payload.Name));
        }

        PinWrightNiagara::FNiagaraResolvedParameterType ResolvedType;
        if (!PinWrightNiagara::ResolveNiagaraParameterType(Payload.Type, ResolvedType))
        {
            return FNiagaraEditError::Make(
                TEXT("INVALID_PARAMETER_TYPE"),
                FString::Printf(TEXT("Niagara parameter type '%s' is not supported by the safe parameter-store edit path."), *Payload.Type));
        }

        FNiagaraVariable Variable(ResolvedType.Definition, FName(*Payload.Name));
        if (Operation == ENiagaraEditOperation::AddParameter)
        {
            if (ExistingByName)
            {
                return FNiagaraEditError::Make(TEXT("PARAMETER_EXISTS"), FString::Printf(TEXT("Parameter '%s' already exists."), *Payload.Name));
            }
            return SetTypedParameterValue(Store, Variable, Payload.Type, ResolvedType, Payload.Value, true);
        }

        if (!ExistingByName)
        {
            return FNiagaraEditError::Make(TEXT("PARAMETER_NOT_FOUND"), FString::Printf(TEXT("Parameter '%s' was not found."), *Payload.Name));
        }
        if (!AreNiagaraParameterTypesCompatible(ExistingByName->GetType(), ResolvedType.Definition))
        {
            // Both sides are the RESOLVED definitions, described as "<name> (<path>)". Printing the
            // existing type's display name against the caller's raw request string produced
            // "exists with type 'NiagaraInt32', not requested type 'NiagaraInt32'" — the comparison
            // is on identity, and two distinct types share a display name often enough (an enum and
            // the int32 struct it is stored as) that the message has to carry the path.
            return FNiagaraEditError::Make(
                TEXT("PARAMETER_TYPE_MISMATCH"),
                FString::Printf(TEXT("Parameter '%s' exists with type '%s', not requested type '%s' (requested as '%s')."),
                    *Payload.Name,
                    *PinWrightNiagara::DescribeNiagaraType(ExistingByName->GetType()),
                    *PinWrightNiagara::DescribeNiagaraType(ResolvedType.Definition),
                    *Payload.Type));
        }
        return SetTypedParameterValue(Store, *ExistingByName, Payload.Type, ResolvedType, Payload.Value, false);
    }

    UClass* ResolveRendererClass(const FString& RendererClassPath)
    {
        return NiagaraEdit::ResolveNiagaraSubclass<UNiagaraRendererProperties>(RendererClassPath, TEXT("Niagara"));
    }

    FNiagaraEditError ApplyRendererMutation(
        const FNiagaraRendererEditPayload& Payload,
        ENiagaraEditOperation Operation,
        FNiagaraResolvedTarget& Target,
        int32& OutRendererIndex,
        FString& OutRendererClass)
    {
        if (!Target.Emitter || !Target.EmitterData)
        {
            return FNiagaraEditError::Make(TEXT("EMITTER_DATA_MISSING"), TEXT("Renderer edits require resolved emitter data."));
        }

        const FGuid VersionGuid = NiagaraEdit::ResolveEmitterVersionGuid(Target);
        if (Operation == ENiagaraEditOperation::AddRenderer)
        {
            UClass* RendererClass = ResolveRendererClass(Payload.RendererClassPath);
            if (!RendererClass)
            {
                return FNiagaraEditError::Make(TEXT("RENDERER_CLASS_NOT_FOUND"), FString::Printf(TEXT("Could not resolve renderer class '%s'."), *Payload.RendererClassPath));
            }
            if (!RendererClass->IsChildOf(UNiagaraRendererProperties::StaticClass()) || RendererClass->HasAnyClassFlags(CLASS_Abstract))
            {
                return FNiagaraEditError::Make(TEXT("INVALID_RENDERER_CLASS"), FString::Printf(TEXT("Class '%s' is not a concrete UNiagaraRendererProperties class."), *RendererClass->GetPathName()));
            }

            UNiagaraRendererProperties* NewRenderer = NewObject<UNiagaraRendererProperties>(Target.Emitter, RendererClass, NAME_None, RF_Transactional);
            if (!NewRenderer)
            {
                return FNiagaraEditError::Make(TEXT("RENDERER_CREATE_FAILED"), FString::Printf(TEXT("Failed to create renderer '%s'."), *RendererClass->GetPathName()));
            }
            NewRenderer->Modify();
            Target.Emitter->AddRenderer(NewRenderer, VersionGuid);
            Target.Renderer = NewRenderer;
            OutRendererIndex = Target.EmitterData->GetRenderers().IndexOfByKey(NewRenderer);
            OutRendererClass = RendererClass->GetPathName();
            return FNiagaraEditError();
        }

        if (!Target.Renderer)
        {
            return FNiagaraEditError::Make(TEXT("RENDERER_NOT_FOUND"), TEXT("Renderer target was not resolved."));
        }

        OutRendererIndex = Payload.Target.Index;
        OutRendererClass = Target.Renderer->GetClass()->GetPathName();
        if (Operation == ENiagaraEditOperation::RemoveRenderer)
        {
            Target.Emitter->RemoveRenderer(Target.Renderer, VersionGuid);
            return FNiagaraEditError();
        }
        if (Operation == ENiagaraEditOperation::MoveRenderer)
        {
            Target.Emitter->MoveRenderer(Target.Renderer, Payload.Target.ToIndex, VersionGuid);
            OutRendererIndex = Payload.Target.ToIndex;
            return FNiagaraEditError();
        }
        return FNiagaraEditError::Make(TEXT("INVALID_OPERATION"), TEXT("Unsupported renderer mutation."));
    }
}

// ---------------------------------------------------------------------------
// NiagaraResetModuleInput — exported implementations (declared in
// NiagaraResetModuleInputHelpers.h).
// ---------------------------------------------------------------------------

namespace NiagaraResetModuleInput
{
    UNiagaraNodeParameterMapSet* FindStackFunctionOverrideNode(UNiagaraNodeFunctionCall& ModuleNode)
    {
        // FNiagaraStackGraphUtilities::GetStackFunctionOverrideNode is not NIAGARAEDITOR_API,
        // so we inline the walk here. UNiagaraNodeParameterMapSet is in a private engine header;
        // use FindObject for a safe runtime class lookup instead of including it.
        UEdGraphPin* ParameterMapPin = GetParameterMapPin(ModuleNode, EGPD_Input);
        if (!ParameterMapPin || ParameterMapPin->LinkedTo.Num() != 1)
        {
            return nullptr;
        }
        UEdGraphNode* OwningNode = ParameterMapPin->LinkedTo[0]->GetOwningNode();
        if (!OwningNode)
        {
            return nullptr;
        }
        static UClass* MapSetClass = FindObject<UClass>(nullptr, TEXT("/Script/NiagaraEditor.NiagaraNodeParameterMapSet"));
        return (MapSetClass && OwningNode->IsA(MapSetClass))
            ? static_cast<UNiagaraNodeParameterMapSet*>(OwningNode)
            : nullptr;
    }

    // Remove only the node(s) that exist *because of* OverridePin. Inline reimpl of
    // FNiagaraStackGraphUtilities::RemoveNodesForStackFunctionInputOverridePin (not NIAGARAEDITOR_API).
    //
    // The bound is the whole point. An override value node's parameter-map INPUT pin is wired to the
    // previous stack node's output pin — SetDynamicInputForFunctionInput and
    // SetLinkedParameterValueForFunctionInput both do exactly that — so a walk that follows every
    // upstream input pin leaves the override after one hop and consumes the rest of the stage's stack:
    // the preceding module calls, their override map-set nodes, and the head UNiagaraNodeInput. That is
    // unrecoverable through the API and it is what B-niagara-reset-module-input-corrupts-stack reported.
    //
    // Only three node kinds can drive an override pin: a literal / data-interface UNiagaraNodeInput,
    // a linked-parameter UNiagaraNodeParameterMapGet, and a dynamic-input UNiagaraNodeFunctionCall
    // (UNiagaraNodeCustomHlsl derives from it). Anything else is left in place rather than guessed at.
    static void RemoveOverrideValueNode(
        UEdGraphPin& OverridePin,
        UNiagaraGraph& Graph,
        TArray<TWeakObjectPtr<UNiagaraDataInterface>>& OutRemovedDI,
        TSet<UEdGraphNode*>& VisitedValueNodes)
    {
        if (OverridePin.LinkedTo.Num() != 1 || OverridePin.LinkedTo[0] == nullptr)
        {
            return;
        }
        UEdGraphNode* ValueNode = OverridePin.LinkedTo[0]->GetOwningNode();
        if (!ValueNode || VisitedValueNodes.Contains(ValueNode))
        {
            return;
        }
        VisitedValueNodes.Add(ValueNode);

        if (UNiagaraNodeInput* InputNode = Cast<UNiagaraNodeInput>(ValueNode))
        {
            // UNiagaraNodeInput::GetDataInterface() is declared public but is not
            // NIAGARAEDITOR_API, so it can't be linked from this DLL; the shared helper reads
            // the backing UPROPERTY through reflection instead.
            if (UNiagaraDataInterface* DI = NiagaraModuleInputDI::GetInputNodeDataInterface(InputNode))
            {
                OutRemovedDI.Add(DI);
            }
            InputNode->Modify();
            Graph.RemoveNode(InputNode);
            return;
        }

        static UClass* MapGetClass = FindObject<UClass>(nullptr, TEXT("/Script/NiagaraEditor.NiagaraNodeParameterMapGet"));
        if (MapGetClass && ValueNode->IsA(MapGetClass))
        {
            ValueNode->Modify();
            Graph.RemoveNode(ValueNode);
            return;
        }

        UNiagaraNodeFunctionCall* DynamicInputNode = Cast<UNiagaraNodeFunctionCall>(ValueNode);
        if (!DynamicInputNode)
        {
            return;
        }

        // A dynamic input that carries overrides of its own owns a UNiagaraNodeParameterMapSet spliced
        // into the map chain ahead of it (GetOrCreateStackFunctionOverrideNode). Tear down only the pins
        // whose namespace names *this* dynamic input, then drop that node only once nothing but the map
        // input and the add pin remain on it — reconnecting the chain across the gap, because that node
        // also feeds the modules downstream of it.
        if (UEdGraphPin* DynamicInputMapPin = GetParameterMapPin(*DynamicInputNode, EGPD_Input))
        {
            static UClass* MapSetClass = FindObject<UClass>(nullptr, TEXT("/Script/NiagaraEditor.NiagaraNodeParameterMapSet"));
            UEdGraphNode* UpstreamNode = (DynamicInputMapPin->LinkedTo.Num() > 0 && DynamicInputMapPin->LinkedTo[0])
                ? DynamicInputMapPin->LinkedTo[0]->GetOwningNode()
                : nullptr;
            UNiagaraNodeParameterMapSet* DynamicInputOverrideNode =
                (MapSetClass && UpstreamNode && UpstreamNode->IsA(MapSetClass))
                    ? static_cast<UNiagaraNodeParameterMapSet*>(UpstreamNode)
                    : nullptr;
            const FName DynamicInputFunctionName(*DynamicInputNode->GetFunctionName());
            if (DynamicInputOverrideNode && !DynamicInputFunctionName.IsNone())
            {
                TArray<UEdGraphPin*> CandidatePins;
                DynamicInputOverrideNode->GetInputPins(CandidatePins);
                TArray<UEdGraphPin*> NestedOverridePins;
                for (UEdGraphPin* CandidatePin : CandidatePins)
                {
                    if (CandidatePin && FNiagaraParameterHandle(CandidatePin->PinName).GetNamespace() == DynamicInputFunctionName)
                    {
                        NestedOverridePins.Add(CandidatePin);
                    }
                }

                DynamicInputOverrideNode->Modify();
                for (UEdGraphPin* NestedPin : NestedOverridePins)
                {
                    RemoveOverrideValueNode(*NestedPin, Graph, OutRemovedDI, VisitedValueNodes);
                    Graph.GetSchema()->BreakPinLinks(*NestedPin, true);
                    DynamicInputOverrideNode->RemovePin(NestedPin);
                }

                TArray<UEdGraphPin*> RemainingInputPins;
                DynamicInputOverrideNode->GetInputPins(RemainingInputPins);
                UEdGraphPin* MapInPin = GetParameterMapPin(*DynamicInputOverrideNode, EGPD_Input);
                UEdGraphPin* MapOutPin = GetParameterMapPin(*DynamicInputOverrideNode, EGPD_Output);
                // Two pins left == the parameter-map input and the add pin, so this override node exists
                // only for the dynamic input being removed. Its output feeds the dynamic input AND at
                // least one downstream stack node; the downstream links must be re-made against the
                // previous stack node's output or the stage's map chain is severed at this point.
                if (RemainingInputPins.Num() == 2
                    && MapInPin && MapInPin->LinkedTo.Num() == 1 && MapInPin->LinkedTo[0]
                    && MapOutPin && MapOutPin->LinkedTo.Num() >= 2)
                {
                    UEdGraphPin* PreviousStackOutputPin = MapInPin->LinkedTo[0];
                    TArray<UEdGraphPin*> DownstreamPins;
                    for (UEdGraphPin* LinkedOutputPin : MapOutPin->LinkedTo)
                    {
                        if (LinkedOutputPin && LinkedOutputPin->GetOwningNode() != DynamicInputNode)
                        {
                            DownstreamPins.Add(LinkedOutputPin);
                        }
                    }
                    MapInPin->BreakAllPinLinks(true);
                    MapOutPin->BreakAllPinLinks(true);
                    Graph.RemoveNode(DynamicInputOverrideNode);
                    for (UEdGraphPin* DownstreamPin : DownstreamPins)
                    {
                        MakeGraphLink(PreviousStackOutputPin, DownstreamPin);
                    }
                }
            }
        }

        DynamicInputNode->Modify();
        Graph.RemoveNode(DynamicInputNode);
    }

    void RemoveOverridePinAndChainedNodes(
        UEdGraphPin& OverridePin,
        UNiagaraGraph& Graph,
        TArray<TWeakObjectPtr<UNiagaraDataInterface>>& OutRemovedDI)
    {
        TSet<UEdGraphNode*> VisitedValueNodes;
        RemoveOverrideValueNode(OverridePin, Graph, OutRemovedDI, VisitedValueNodes);

        Graph.GetSchema()->BreakPinLinks(OverridePin, true);

        UNiagaraNode* OverrideNode = Cast<UNiagaraNode>(OverridePin.GetOwningNode());
        if (OverrideNode)
        {
            OverrideNode->Modify();
            OverrideNode->RemovePin(&OverridePin);
        }
    }

    bool ClearModuleInputOverride(
        UNiagaraNodeFunctionCall& ModuleNode,
        const FNiagaraParameterHandle& AliasedInputHandle,
        UNiagaraGraph& Graph)
    {
        // Find this input's override pin on the module's override node (if any) and tear it
        // down — literal default, dynamic-input chain, or stale link. Shared by the linked-
        // parameter write path (which must clear any prior override before
        // SetLinkedParameterValueForFunctionInput, whose checkf rejects a pre-linked pin).
        UNiagaraNodeParameterMapSet* OverrideNode = FindStackFunctionOverrideNode(ModuleNode);
        if (!OverrideNode)
        {
            return false;
        }
        TArray<UEdGraphPin*> OverridePins;
        OverrideNode->GetInputPins(OverridePins);
        const FName AliasedName = AliasedInputHandle.GetParameterHandleString();
        UEdGraphPin** PinPtr = OverridePins.FindByPredicate(
            [&AliasedName](const UEdGraphPin* Pin)
            {
                return Pin && Pin->PinName == AliasedName;
            });
        if (!PinPtr)
        {
            return false;
        }
        TArray<TWeakObjectPtr<UNiagaraDataInterface>> RemovedDI;
        RemoveOverridePinAndChainedNodes(**PinPtr, Graph, RemovedDI);
        return true;
    }
} // namespace NiagaraResetModuleInput

namespace
{
    // Pre-transaction dirty baseline, restored when the edit is refused.
    //
    // The module handlers open their FScopedTransaction and run ModifyResolvedTarget /
    // Graph->Modify() before ApplyModuleMutation gets to run its guards, so a refusal returned
    // from there — INVALID_STACK, PARAMETER_TYPE_MISMATCH, UNSUPPORTED_INPUT_VALUE,
    // MODULE_INPUT_OVERRIDE_LINKED — leaves the asset dirty having changed nothing. Ending or
    // cancelling the transaction does not undo that: UTransBuffer::Cancel only pops the record
    // off the undo buffer, it never replays it.
    //
    // Sampling the packages the transaction is about to dirty and restoring them on refusal
    // covers every refusal code on the path at once, present and future, instead of auditing
    // each error return. Only packages that were already clean are restored, so dirt from a
    // cold load's PostLoad or from a concurrent editor edit is never cleared.
    class FNiagaraCleanPackageBaseline
    {
    public:
        explicit FNiagaraCleanPackageBaseline(const FNiagaraResolvedTarget& Target)
        {
            // Every object ModifyResolvedTarget touches, plus the graph the handlers Modify()
            // directly — a module edit spans two packages when the emitter is its own asset.
            AddIfClean(Target.Asset);
            AddIfClean(Target.System);
            AddIfClean(Target.Emitter);
            AddIfClean(Target.ReflectedObject);
            AddIfClean(Target.Renderer);
            AddIfClean(Target.Graph);
        }

        void RestoreAfterRefusal() const
        {
            for (UPackage* Package : CleanPackages)
            {
                Package->SetDirtyFlag(false);
            }
        }

    private:
        void AddIfClean(const UObject* Object)
        {
            UPackage* Package = Object ? Object->GetOutermost() : nullptr;
            if (Package && !Package->IsDirty())
            {
                CleanPackages.AddUnique(Package);
            }
        }

        TArray<UPackage*> CleanPackages;
    };
}

REGISTER_RPC_HANDLER("niagara.set_property", "niagara", "Set one reflected property on a resolved Niagara target.",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath", "path", "Path to the Niagara System or Niagara Emitter asset"),
        RPC_PARAM_REQ("target", "object", "Target descriptor with kind plus emitter/index/script/node fields as needed"),
        RPC_PARAM_REQ("propertyPath", "string", "Reflected property path on the resolved target"),
        RPC_PARAM_REQ("value", "any", "JSON value to assign"),
        RPC_PARAM_OPT("compile", "boolean", "Compile after edit"),
        RPC_PARAM_OPT("save", "boolean", "Save after edit")
    ))
{
    FNiagaraPropertyEditPayload Payload;
    if (SendNiagaraEditError(Ctx, NiagaraEdit::ParsePropertyPayload(Ctx.GetRawPayload(), Payload))) return true;

    FNiagaraResolvedTarget Target;
    FProperty* Property = nullptr;
    void* Container = nullptr;
    if (SendNiagaraEditError(Ctx, NiagaraEdit::ValidatePropertyPayload(Payload, Target, Property, Container))) return true;

    {
        FScopedTransaction Transaction(FText::FromString(TEXT("MCP: niagara.set_property")));
        NiagaraEdit::ModifyResolvedTarget(Target);

        FString ApplyError;
        if (!ApplyJsonValueToProperty(Container, Property, Payload.Value, ApplyError))
        {
            Ctx.SendError(TEXT("PROPERTY_SET_FAILED"), FString::Printf(TEXT("Failed to set '%s': %s"), *Payload.PropertyPath, *ApplyError));
            return true;
        }

        NiagaraEdit::NotifyNiagaraObjectChanged(Target, Property);
    }

    bool bCompiled = false;
    bool bSaved = false;
    NiagaraEdit::FinalizeNiagaraEdit(Target, Payload.Options, bCompiled, bSaved);

    TSharedPtr<FJsonObject> Result = NiagaraEdit::MakeMutationResult(TEXT("set_property"), Target, Payload.Options, bCompiled, bSaved);
    Result->SetStringField(TEXT("targetKind"), NiagaraEdit::TargetKindToString(Payload.Target.Kind));
    Result->SetStringField(TEXT("propertyPath"), Payload.PropertyPath);
    Ctx.SendSuccess(Result);
    return true;
}

REGISTER_RPC_HANDLER("niagara.set_parameter", "niagara", "Set one Niagara parameter-store value.",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath", "path", "Path to the Niagara System or Niagara Emitter asset"),
        RPC_PARAM_REQ("scope", "string", "Parameter scope: user, systemSpawnRapidIteration, systemUpdateRapidIteration, rendererBindings, spawnRapidIteration, or updateRapidIteration"),
        RPC_PARAM_REQ("name", "string", "Parameter name"),
        RPC_PARAM_REQ("type", "string", "Parameter type, as a string: either a short alias (int32, float, bool, vec2, vec3, position, color, id, spawninfo, half2) or the canonical name niagara.inspect prints (NiagaraInt32, NiagaraFloat, Vector3f, LinearColor, NiagaraPosition). Send inspect's type.name, not its type object"),
        RPC_PARAM_REQ("value", "any", "Value to assign"),
        RPC_PARAM_OPT("emitter", "string", "Emitter name for emitter-scoped stores. Required by rendererBindings, spawnRapidIteration and updateRapidIteration when assetPath is a Niagara System; omit for a Niagara Emitter asset (its own data is used) and for the user / systemSpawnRapidIteration / systemUpdateRapidIteration scopes, which reject it with INVALID_ARGUMENT because their stores live on the system"),
        RPC_PARAM_OPT("compile", "boolean", "Compile after edit"),
        RPC_PARAM_OPT("save", "boolean", "Save after edit")
    ))
{
    FNiagaraParameterEditPayload Payload;
    if (SendNiagaraEditError(Ctx, NiagaraEdit::ParseParameterPayload(Ctx.GetRawPayload(), ENiagaraEditOperation::SetParameter, Payload))) return true;

    FNiagaraResolvedTarget Target;
    if (SendNiagaraEditError(Ctx, NiagaraEdit::ValidateParameterPayload(Payload, ENiagaraEditOperation::SetParameter, Target))) return true;

    {
        FScopedTransaction Transaction(FText::FromString(TEXT("MCP: niagara.set_parameter")));
        NiagaraEdit::ModifyResolvedTarget(Target);
        if (SendNiagaraEditError(Ctx, ApplyParameterMutation(Payload, ENiagaraEditOperation::SetParameter, Target))) return true;
        NiagaraEdit::NotifyNiagaraObjectChanged(Target, nullptr);
    }

    bool bCompiled = false;
    bool bSaved = false;
    NiagaraEdit::FinalizeNiagaraEdit(Target, Payload.Options, bCompiled, bSaved);

    TSharedPtr<FJsonObject> Result = NiagaraEdit::MakeMutationResult(TEXT("set_parameter"), Target, Payload.Options, bCompiled, bSaved);
    Result->SetStringField(TEXT("scope"), Payload.Scope);
    Result->SetStringField(TEXT("name"), Payload.Name);
    Result->SetStringField(TEXT("type"), Payload.Type);
    Ctx.SendSuccess(Result);
    return true;
}

REGISTER_RPC_HANDLER("niagara.add_parameter", "niagara", "Add one Niagara parameter-store value.",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath", "path", "Path to the Niagara System or Niagara Emitter asset"),
        RPC_PARAM_REQ("scope", "string", "Parameter scope"),
        RPC_PARAM_REQ("name", "string", "Parameter name"),
        RPC_PARAM_REQ("type", "string", "Parameter type, as a string: either a short alias (int32, float, bool, vec2, vec3, position, color, id, spawninfo, half2) or the canonical name niagara.inspect prints (NiagaraInt32, NiagaraFloat, Vector3f, LinearColor, NiagaraPosition). Send inspect's type.name, not its type object"),
        RPC_PARAM_REQ("defaultValue", "any", "Initial value"),
        RPC_PARAM_OPT("emitter", "string", "Emitter name for emitter-scoped stores. Required by rendererBindings, spawnRapidIteration and updateRapidIteration when assetPath is a Niagara System; omit for a Niagara Emitter asset (its own data is used) and for the user / systemSpawnRapidIteration / systemUpdateRapidIteration scopes, which reject it with INVALID_ARGUMENT because their stores live on the system"),
        RPC_PARAM_OPT("compile", "boolean", "Compile after edit"),
        RPC_PARAM_OPT("save", "boolean", "Save after edit")
    ))
{
    FNiagaraParameterEditPayload Payload;
    if (SendNiagaraEditError(Ctx, NiagaraEdit::ParseParameterPayload(Ctx.GetRawPayload(), ENiagaraEditOperation::AddParameter, Payload))) return true;

    FNiagaraResolvedTarget Target;
    if (SendNiagaraEditError(Ctx, NiagaraEdit::ValidateParameterPayload(Payload, ENiagaraEditOperation::AddParameter, Target))) return true;

    {
        FScopedTransaction Transaction(FText::FromString(TEXT("MCP: niagara.add_parameter")));
        NiagaraEdit::ModifyResolvedTarget(Target);
        if (SendNiagaraEditError(Ctx, ApplyParameterMutation(Payload, ENiagaraEditOperation::AddParameter, Target))) return true;
        NiagaraEdit::NotifyNiagaraObjectChanged(Target, nullptr);
    }

    bool bCompiled = false;
    bool bSaved = false;
    NiagaraEdit::FinalizeNiagaraEdit(Target, Payload.Options, bCompiled, bSaved);

    TSharedPtr<FJsonObject> Result = NiagaraEdit::MakeMutationResult(TEXT("add_parameter"), Target, Payload.Options, bCompiled, bSaved);
    Result->SetStringField(TEXT("scope"), Payload.Scope);
    Result->SetStringField(TEXT("name"), Payload.Name);
    Result->SetStringField(TEXT("type"), Payload.Type);
    Ctx.SendSuccess(Result);
    return true;
}

REGISTER_RPC_HANDLER("niagara.remove_parameter", "niagara", "Remove one Niagara parameter-store value.",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath", "path", "Path to the Niagara System or Niagara Emitter asset"),
        RPC_PARAM_REQ("scope", "string", "Parameter scope"),
        RPC_PARAM_REQ("name", "string", "Parameter name"),
        RPC_PARAM_OPT("emitter", "string", "Emitter name for emitter-scoped stores. Required by rendererBindings, spawnRapidIteration and updateRapidIteration when assetPath is a Niagara System; omit for a Niagara Emitter asset (its own data is used) and for the user / systemSpawnRapidIteration / systemUpdateRapidIteration scopes, which reject it with INVALID_ARGUMENT because their stores live on the system"),
        RPC_PARAM_OPT("compile", "boolean", "Compile after edit"),
        RPC_PARAM_OPT("save", "boolean", "Save after edit")
    ))
{
    FNiagaraParameterEditPayload Payload;
    if (SendNiagaraEditError(Ctx, NiagaraEdit::ParseParameterPayload(Ctx.GetRawPayload(), ENiagaraEditOperation::RemoveParameter, Payload))) return true;

    FNiagaraResolvedTarget Target;
    if (SendNiagaraEditError(Ctx, NiagaraEdit::ValidateParameterPayload(Payload, ENiagaraEditOperation::RemoveParameter, Target))) return true;

    {
        FScopedTransaction Transaction(FText::FromString(TEXT("MCP: niagara.remove_parameter")));
        NiagaraEdit::ModifyResolvedTarget(Target);
        if (SendNiagaraEditError(Ctx, ApplyParameterMutation(Payload, ENiagaraEditOperation::RemoveParameter, Target))) return true;
        NiagaraEdit::NotifyNiagaraObjectChanged(Target, nullptr);
    }

    bool bCompiled = false;
    bool bSaved = false;
    NiagaraEdit::FinalizeNiagaraEdit(Target, Payload.Options, bCompiled, bSaved);

    TSharedPtr<FJsonObject> Result = NiagaraEdit::MakeMutationResult(TEXT("remove_parameter"), Target, Payload.Options, bCompiled, bSaved);
    Result->SetStringField(TEXT("scope"), Payload.Scope);
    Result->SetStringField(TEXT("name"), Payload.Name);
    Ctx.SendSuccess(Result);
    return true;
}

REGISTER_RPC_HANDLER("niagara.add_renderer", "niagara", "Add one Niagara renderer to an emitter.",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath", "path", "Path to the Niagara System or Niagara Emitter asset"),
        RPC_PARAM_REQ("rendererClassPath", "classref", "Renderer properties class path"),
        RPC_PARAM_OPT("target", "object", "Emitter target descriptor"),
        RPC_PARAM_OPT("emitter", "string", "Emitter name for Niagara System assets"),
        RPC_PARAM_OPT("compile", "boolean", "Compile after edit"),
        RPC_PARAM_OPT("save", "boolean", "Save after edit")
    ))
{
    FNiagaraRendererEditPayload Payload;
    if (SendNiagaraEditError(Ctx, NiagaraEdit::ParseRendererPayload(Ctx.GetRawPayload(), ENiagaraEditOperation::AddRenderer, Payload))) return true;

    FNiagaraResolvedTarget Target;
    if (SendNiagaraEditError(Ctx, NiagaraEdit::ValidateRendererPayload(Payload, ENiagaraEditOperation::AddRenderer, Target))) return true;

    int32 RendererIndex = INDEX_NONE;
    FString RendererClass;
    {
        FScopedTransaction Transaction(FText::FromString(TEXT("MCP: niagara.add_renderer")));
        NiagaraEdit::ModifyResolvedTarget(Target);
        if (SendNiagaraEditError(Ctx, ApplyRendererMutation(Payload, ENiagaraEditOperation::AddRenderer, Target, RendererIndex, RendererClass))) return true;
        NiagaraEdit::NotifyNiagaraObjectChanged(Target, nullptr);
    }

    bool bCompiled = false;
    bool bSaved = false;
    NiagaraEdit::FinalizeNiagaraEdit(Target, Payload.Options, bCompiled, bSaved);

    TSharedPtr<FJsonObject> Result = NiagaraEdit::MakeMutationResult(TEXT("add_renderer"), Target, Payload.Options, bCompiled, bSaved);
    Result->SetNumberField(TEXT("index"), RendererIndex);
    Result->SetStringField(TEXT("rendererClass"), RendererClass);
    Ctx.SendSuccess(Result);
    return true;
}

REGISTER_RPC_HANDLER("niagara.remove_renderer", "niagara", "Remove one Niagara renderer by index.",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath", "path", "Path to the Niagara System or Niagara Emitter asset"),
        RPC_PARAM_REQ("index", "integer", "Renderer index"),
        RPC_PARAM_OPT("target", "object", "Renderer target descriptor"),
        RPC_PARAM_OPT("emitter", "string", "Emitter name for Niagara System assets"),
        RPC_PARAM_OPT("compile", "boolean", "Compile after edit"),
        RPC_PARAM_OPT("save", "boolean", "Save after edit")
    ))
{
    FNiagaraRendererEditPayload Payload;
    if (SendNiagaraEditError(Ctx, NiagaraEdit::ParseRendererOrdinalPayload(Ctx.GetRawPayload(), ENiagaraEditOperation::RemoveRenderer, Payload))) return true;

    FNiagaraResolvedTarget Target;
    if (SendNiagaraEditError(Ctx, NiagaraEdit::ValidateRendererPayload(Payload, ENiagaraEditOperation::RemoveRenderer, Target))) return true;

    int32 RendererIndex = INDEX_NONE;
    FString RendererClass;
    {
        FScopedTransaction Transaction(FText::FromString(TEXT("MCP: niagara.remove_renderer")));
        NiagaraEdit::ModifyResolvedTarget(Target);
        if (SendNiagaraEditError(Ctx, ApplyRendererMutation(Payload, ENiagaraEditOperation::RemoveRenderer, Target, RendererIndex, RendererClass))) return true;
        NiagaraEdit::NotifyNiagaraObjectChanged(Target, nullptr);
    }

    bool bCompiled = false;
    bool bSaved = false;
    NiagaraEdit::FinalizeNiagaraEdit(Target, Payload.Options, bCompiled, bSaved);

    TSharedPtr<FJsonObject> Result = NiagaraEdit::MakeMutationResult(TEXT("remove_renderer"), Target, Payload.Options, bCompiled, bSaved);
    Result->SetNumberField(TEXT("index"), RendererIndex);
    Result->SetStringField(TEXT("rendererClass"), RendererClass);
    Ctx.SendSuccess(Result);
    return true;
}

REGISTER_RPC_HANDLER("niagara.move_renderer", "niagara", "Move one Niagara renderer from index to toIndex.",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath", "path", "Path to the Niagara System or Niagara Emitter asset"),
        RPC_PARAM_REQ("index", "integer", "Renderer index"),
        RPC_PARAM_REQ("toIndex", "integer", "Destination renderer index"),
        RPC_PARAM_OPT("target", "object", "Renderer target descriptor"),
        RPC_PARAM_OPT("emitter", "string", "Emitter name for Niagara System assets"),
        RPC_PARAM_OPT("compile", "boolean", "Compile after edit"),
        RPC_PARAM_OPT("save", "boolean", "Save after edit")
    ))
{
    FNiagaraRendererEditPayload Payload;
    if (SendNiagaraEditError(Ctx, NiagaraEdit::ParseRendererOrdinalPayload(Ctx.GetRawPayload(), ENiagaraEditOperation::MoveRenderer, Payload))) return true;

    FNiagaraResolvedTarget Target;
    if (SendNiagaraEditError(Ctx, NiagaraEdit::ValidateRendererPayload(Payload, ENiagaraEditOperation::MoveRenderer, Target))) return true;

    int32 RendererIndex = INDEX_NONE;
    FString RendererClass;
    {
        FScopedTransaction Transaction(FText::FromString(TEXT("MCP: niagara.move_renderer")));
        NiagaraEdit::ModifyResolvedTarget(Target);
        if (SendNiagaraEditError(Ctx, ApplyRendererMutation(Payload, ENiagaraEditOperation::MoveRenderer, Target, RendererIndex, RendererClass))) return true;
        NiagaraEdit::NotifyNiagaraObjectChanged(Target, nullptr);
    }

    bool bCompiled = false;
    bool bSaved = false;
    NiagaraEdit::FinalizeNiagaraEdit(Target, Payload.Options, bCompiled, bSaved);

    TSharedPtr<FJsonObject> Result = NiagaraEdit::MakeMutationResult(TEXT("move_renderer"), Target, Payload.Options, bCompiled, bSaved);
    Result->SetNumberField(TEXT("fromIndex"), Payload.Target.Index);
    Result->SetNumberField(TEXT("toIndex"), RendererIndex);
    Result->SetStringField(TEXT("rendererClass"), RendererClass);
    Ctx.SendSuccess(Result);
    return true;
}

REGISTER_RPC_HANDLER("niagara.add_module", "niagara", "Add one Niagara stack module to a graph.",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath", "path", "Path to the Niagara System or Niagara Emitter asset"),
        RPC_PARAM_REQ("modulePath", "path", "Niagara module script path"),
        RPC_PARAM_OPT("target", "object", "Graph target descriptor"),
        RPC_PARAM_OPT("emitter", "string", "Emitter name for Niagara System assets"),
        RPC_PARAM_OPT("scriptUsage", "string", "Script usage"),
        RPC_PARAM_OPT("compile", "boolean", "Compile after edit"),
        RPC_PARAM_OPT("save", "boolean", "Save after edit")
    ))
{
    FNiagaraModuleEditPayload Payload;
    if (SendNiagaraEditError(Ctx, NiagaraEdit::ParseModulePayload(Ctx.GetRawPayload(), ENiagaraEditOperation::AddModule, Payload))) return true;

    FNiagaraResolvedTarget Target;
    if (SendNiagaraEditError(Ctx, NiagaraEdit::ValidateModulePayload(Payload, ENiagaraEditOperation::AddModule, Target))) return true;

    FString NodeId;
    int32 ModuleIndex = INDEX_NONE;
    {
        FScopedTransaction Transaction(FText::FromString(TEXT("MCP: niagara.add_module")));
        NiagaraEdit::ModifyResolvedTarget(Target);
        if (Target.Graph)
        {
            Target.Graph->Modify();
        }
        if (SendNiagaraEditError(Ctx, ApplyModuleMutation(Payload, ENiagaraEditOperation::AddModule, Target, false, NodeId, ModuleIndex))) return true;
        NotifyNiagaraGraphChanged(Target);
    }

    bool bCompiled = false;
    bool bSaved = false;
    NiagaraEdit::FinalizeNiagaraEdit(Target, Payload.Options, bCompiled, bSaved);

    TSharedPtr<FJsonObject> Result = NiagaraEdit::MakeMutationResult(TEXT("add_module"), Target, Payload.Options, bCompiled, bSaved);
    Result->SetStringField(TEXT("modulePath"), Payload.ModulePath);
    Result->SetStringField(TEXT("nodeId"), NodeId);
    Result->SetNumberField(TEXT("index"), ModuleIndex);
    Ctx.SendSuccess(Result);
    return true;
}

REGISTER_RPC_HANDLER("niagara.remove_module", "niagara", "Remove one Niagara stack module.",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath", "path", "Path to the Niagara System or Niagara Emitter asset"),
        RPC_PARAM_REQ("entryId", "string", "Module entry id or node id, or the owner-qualified entryKey 'owner:id' (a bare id is unique only within one emitter)"),
        RPC_PARAM_OPT("target", "object", "Module target descriptor"),
        RPC_PARAM_OPT("emitter", "string", "Emitter name for Niagara System assets"),
        RPC_PARAM_OPT("scriptUsage", "string", "Script usage"),
        RPC_PARAM_OPT("compile", "boolean", "Compile after edit"),
        RPC_PARAM_OPT("save", "boolean", "Save after edit")
    ))
{
    FNiagaraModuleEditPayload Payload;
    if (SendNiagaraEditError(Ctx, NiagaraEdit::ParseModulePayload(Ctx.GetRawPayload(), ENiagaraEditOperation::RemoveModule, Payload))) return true;

    FNiagaraResolvedTarget Target;
    if (SendNiagaraEditError(Ctx, NiagaraEdit::ValidateModulePayload(Payload, ENiagaraEditOperation::RemoveModule, Target))) return true;

    FString NodeId;
    int32 ModuleIndex = INDEX_NONE;
    {
        FScopedTransaction Transaction(FText::FromString(TEXT("MCP: niagara.remove_module")));
        NiagaraEdit::ModifyResolvedTarget(Target);
        if (Target.Graph)
        {
            Target.Graph->Modify();
        }
        if (SendNiagaraEditError(Ctx, ApplyModuleMutation(Payload, ENiagaraEditOperation::RemoveModule, Target, false, NodeId, ModuleIndex))) return true;
        NotifyNiagaraGraphChanged(Target);
    }

    bool bCompiled = false;
    bool bSaved = false;
    NiagaraEdit::FinalizeNiagaraEdit(Target, Payload.Options, bCompiled, bSaved);

    TSharedPtr<FJsonObject> Result = NiagaraEdit::MakeMutationResult(TEXT("remove_module"), Target, Payload.Options, bCompiled, bSaved);
    Result->SetStringField(TEXT("entryId"), Payload.Target.EntryId);
    Result->SetStringField(TEXT("nodeId"), NodeId);
    Result->SetNumberField(TEXT("index"), ModuleIndex);
    Ctx.SendSuccess(Result);
    return true;
}

REGISTER_RPC_HANDLER("niagara.move_module", "niagara", "Move one Niagara stack module.",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath", "path", "Path to the Niagara System or Niagara Emitter asset"),
        RPC_PARAM_REQ("entryId", "string", "Module entry id or node id, or the owner-qualified entryKey 'owner:id' (a bare id is unique only within one emitter)"),
        RPC_PARAM_REQ("toIndex", "integer", "Destination module index"),
        RPC_PARAM_OPT("target", "object", "Module target descriptor"),
        RPC_PARAM_OPT("emitter", "string", "Emitter name for Niagara System assets"),
        RPC_PARAM_OPT("scriptUsage", "string", "Script usage"),
        RPC_PARAM_OPT("compile", "boolean", "Compile after edit"),
        RPC_PARAM_OPT("save", "boolean", "Save after edit")
    ))
{
    FNiagaraModuleEditPayload Payload;
    if (SendNiagaraEditError(Ctx, NiagaraEdit::ParseModulePayload(Ctx.GetRawPayload(), ENiagaraEditOperation::MoveModule, Payload))) return true;

    FNiagaraResolvedTarget Target;
    if (SendNiagaraEditError(Ctx, NiagaraEdit::ValidateModulePayload(Payload, ENiagaraEditOperation::MoveModule, Target))) return true;

    FString NodeId;
    int32 ModuleIndex = INDEX_NONE;
    {
        FScopedTransaction Transaction(FText::FromString(TEXT("MCP: niagara.move_module")));
        NiagaraEdit::ModifyResolvedTarget(Target);
        if (Target.Graph)
        {
            Target.Graph->Modify();
        }
        if (SendNiagaraEditError(Ctx, ApplyModuleMutation(Payload, ENiagaraEditOperation::MoveModule, Target, false, NodeId, ModuleIndex))) return true;
        NotifyNiagaraGraphChanged(Target);
    }

    bool bCompiled = false;
    bool bSaved = false;
    NiagaraEdit::FinalizeNiagaraEdit(Target, Payload.Options, bCompiled, bSaved);

    TSharedPtr<FJsonObject> Result = NiagaraEdit::MakeMutationResult(TEXT("move_module"), Target, Payload.Options, bCompiled, bSaved);
    Result->SetStringField(TEXT("entryId"), Payload.Target.EntryId);
    Result->SetStringField(TEXT("nodeId"), NodeId);
    Result->SetNumberField(TEXT("toIndex"), ModuleIndex);
    Ctx.SendSuccess(Result);
    return true;
}

REGISTER_RPC_HANDLER("niagara.set_stack_enabled", "niagara", "Enable or disable one Niagara stack module entry.",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath", "path", "Path to the Niagara System or Niagara Emitter asset"),
        RPC_PARAM_REQ("entryId", "string", "Module entry id or node id, or the owner-qualified entryKey 'owner:id' (a bare id is unique only within one emitter)"),
        RPC_PARAM_REQ("enabled", "boolean", "Enabled state"),
        RPC_PARAM_OPT("target", "object", "Module target descriptor"),
        RPC_PARAM_OPT("emitter", "string", "Emitter name for Niagara System assets"),
        RPC_PARAM_OPT("scriptUsage", "string", "Script usage"),
        RPC_PARAM_OPT("compile", "boolean", "Compile after edit"),
        RPC_PARAM_OPT("save", "boolean", "Save after edit")
    ))
{
    FNiagaraModuleEditPayload Payload;
    if (SendNiagaraEditError(Ctx, NiagaraEdit::ParseModulePayload(Ctx.GetRawPayload(), ENiagaraEditOperation::SetStackEnabled, Payload))) return true;

    FNiagaraResolvedTarget Target;
    if (SendNiagaraEditError(Ctx, NiagaraEdit::ValidateModulePayload(Payload, ENiagaraEditOperation::SetStackEnabled, Target))) return true;

    bool bEnabled = false;
    Ctx.GetRawPayload()->TryGetBoolField(TEXT("enabled"), bEnabled);

    FString NodeId;
    int32 ModuleIndex = INDEX_NONE;
    {
        FScopedTransaction Transaction(FText::FromString(TEXT("MCP: niagara.set_stack_enabled")));
        NiagaraEdit::ModifyResolvedTarget(Target);
        if (Target.Graph)
        {
            Target.Graph->Modify();
        }
        if (SendNiagaraEditError(Ctx, ApplyModuleMutation(Payload, ENiagaraEditOperation::SetStackEnabled, Target, bEnabled, NodeId, ModuleIndex))) return true;
        NotifyNiagaraGraphChanged(Target);
    }

    bool bCompiled = false;
    bool bSaved = false;
    NiagaraEdit::FinalizeNiagaraEdit(Target, Payload.Options, bCompiled, bSaved);

    TSharedPtr<FJsonObject> Result = NiagaraEdit::MakeMutationResult(TEXT("set_stack_enabled"), Target, Payload.Options, bCompiled, bSaved);
    Result->SetStringField(TEXT("entryId"), Payload.Target.EntryId);
    Result->SetStringField(TEXT("nodeId"), NodeId);
    Result->SetBoolField(TEXT("enabled"), bEnabled);
    Result->SetNumberField(TEXT("index"), ModuleIndex);
    Ctx.SendSuccess(Result);
    return true;
}

REGISTER_RPC_HANDLER("niagara.set_module_input", "niagara", "Set one Niagara module input value.",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath", "path", "Path to the Niagara System or Niagara Emitter asset"),
        RPC_PARAM_REQ("entryId", "string", "Module entry id or node id, or the owner-qualified entryKey 'owner:id' (a bare id is unique only within one emitter)"),
        RPC_PARAM_REQ("inputName", "string", "Module input name"),
        RPC_PARAM_REQ("value", "any", "Literal value, or { link: \"User.X\" } to bind an existing parameter, or { dynamicInput: \"/Path/Script.Script\" } to assign a dynamic-input node. A link or dynamicInput REPLACES whatever the input's override pin already carried (a dynamic-input chain, a parameter binding or a literal) without an opt-in, because both take effect as asked; the response reports what was destroyed as replacedOverride {valueMode, source, value}. Only a literal over an inbound link is refused, since that write would never be read - see breakExistingLink."),
        RPC_PARAM_OPT("target", "object", "Module target descriptor"),
        RPC_PARAM_OPT("emitter", "string", "Emitter name for Niagara System assets"),
        RPC_PARAM_OPT("scriptUsage", "string", "Script usage"),
        RPC_PARAM_OPT("breakExistingLink", "boolean", "Literal values only. A literal write is refused with MODULE_INPUT_OVERRIDE_LINKED when the input's override pin already has an inbound link (dynamic input, parameter binding, data interface), because the graph reads that link and never the pin default. Pass true to break the link and delete its orphaned upstream chain first; the response then carries replacedOverride naming what the literal displaced. Defaults to false."),
        RPC_PARAM_OPT("compile", "boolean", "Compile after edit"),
        RPC_PARAM_OPT("save", "boolean", "Save after edit")
    ))
{
    FNiagaraModuleEditPayload Payload;
    if (SendNiagaraEditError(Ctx, NiagaraEdit::ParseModulePayload(Ctx.GetRawPayload(), ENiagaraEditOperation::SetModuleInput, Payload))) return true;

    FNiagaraResolvedTarget Target;
    if (SendNiagaraEditError(Ctx, NiagaraEdit::ValidateModulePayload(Payload, ENiagaraEditOperation::SetModuleInput, Target))) return true;

    FString PinId;
    int32 ModuleIndex = INDEX_NONE;
    FString LinkedParameter;
    FString LinkedParameterType;
    FString WrittenValue;
    FString DynamicInput;
    FNiagaraOverrideReplacement ReplacedOverride;
    PinWrightNiagara::FRapidIterationWriteThrough RapidIteration;
    ReplacedOverride.bBreakExistingLink = Ctx.GetBool(TEXT("breakExistingLink"), false);
    // Sampled after ValidateModulePayload resolved the target — so dirt a cold load's PostLoad
    // produced belongs to the baseline and survives the restore — and before the transaction
    // below dirties anything of its own.
    const FNiagaraCleanPackageBaseline CleanBaseline(Target);
    FNiagaraEditError MutationError;
    {
        FScopedTransaction Transaction(FText::FromString(TEXT("MCP: niagara.set_module_input")));
        NiagaraEdit::ModifyResolvedTarget(Target);
        if (Target.Graph)
        {
            Target.Graph->Modify();
        }
        MutationError = ApplyModuleMutation(Payload, ENiagaraEditOperation::SetModuleInput, Target, false, PinId, ModuleIndex, nullptr, &LinkedParameter, &LinkedParameterType, &WrittenValue, &DynamicInput, &ReplacedOverride, &RapidIteration);
        if (!MutationError.HasError())
        {
            NotifyNiagaraGraphChanged(Target);
        }
    }
    if (MutationError.HasError())
    {
        // The write was refused, so the asset must not be left queued for the next save prompt
        // (B-niagara-refused-edit-dirties-package). Restored outside the transaction scope so
        // nothing the transaction does on close can re-dirty behind the restore.
        CleanBaseline.RestoreAfterRefusal();
        SendNiagaraEditError(Ctx, MutationError);
        return true;
    }

    bool bCompiled = false;
    bool bSaved = false;
    NiagaraEdit::FinalizeNiagaraEdit(Target, Payload.Options, bCompiled, bSaved);

    TSharedPtr<FJsonObject> Result = NiagaraEdit::MakeMutationResult(TEXT("set_module_input"), Target, Payload.Options, bCompiled, bSaved);
    Result->SetStringField(TEXT("entryId"), Payload.Target.EntryId);
    Result->SetStringField(TEXT("inputName"), Payload.InputName);
    Result->SetStringField(TEXT("pinId"), PinId);
    Result->SetNumberField(TEXT("index"), ModuleIndex);
    // Report whether the input was bound to a parameter (linked) vs written as a literal,
    // so callers can confirm the live binding the desync-prone literal path cannot produce.
    const bool bLinked = !LinkedParameter.IsEmpty();
    Result->SetBoolField(TEXT("linked"), bLinked);
    if (bLinked)
    {
        Result->SetStringField(TEXT("parameter"), LinkedParameter);
        Result->SetStringField(TEXT("parameterType"), LinkedParameterType);
    }
    else if (!DynamicInput.IsEmpty())
    {
        // A dynamic-input chain was assigned; report the script the override pin now reads from.
        Result->SetStringField(TEXT("dynamicInput"), DynamicInput);
    }
    else if (!WrittenValue.IsEmpty())
    {
        // Echo the written override-pin value (see OutWrittenValue doc). Reached only when the
        // override pin carries this literal — a pin with an inbound link is refused above unless
        // breakExistingLink cleared it — so the echo names a value the graph actually reads.
        Result->SetStringField(TEXT("value"), WrittenValue);
    }

    // A literal write also reconciles the rapid-iteration constant for this input when one
    // exists, because that constant — not the override pin — is what a compiled simulation
    // reads for an input that already had one. Reported on every literal write, shadow or not,
    // so "no constant existed" is a stated result rather than an absent field the caller has to
    // interpret. Omitted for the link / dynamicInput modes: those drive the input from the pin's
    // inbound connection, which no rapid-iteration constant can express.
    if (TSharedPtr<FJsonObject> Constant = MakeRapidIterationReport(RapidIteration))
    {
        Result->SetObjectField(TEXT("rapidIteration"), Constant);
    }

    // The write can land on the correct override pin of a real input and still change nothing:
    // if the input's only consumers sit on a static-switch branch the switch does not take, the
    // compiler drops it as dead code. The write is kept (a caller may be staging a value before
    // flipping the switch), but a success that reads identical to a live one is what made this
    // an invisible no-op, so the gate is named here as well as on the readback.
    if (Target.ModuleNode)
    {
        NiagaraEdit::FModuleInputGate Gate;
        const bool bGated = NiagaraEdit::FindModuleInputGate(*Target.ModuleNode, FName(*Payload.InputName), Gate);
        Result->SetBoolField(TEXT("reachable"), !bGated);
        if (bGated)
        {
            Result->SetObjectField(TEXT("gatedBy"), NiagaraEdit::MakeGatedByJson(Gate));
        }
    }

    // Every value mode replaces whatever the override pin already carried, so every value mode
    // discloses it here rather than leaving the caller to infer the loss from silence. What was
    // deleted — a dynamic-input chain (possibly nested), a parameter binding, or a literal — is
    // not recoverable from the rest of the response, and for the literal mode the caller
    // authorized the destruction with breakExistingLink but still cannot see it from the echo.
    if (!ReplacedOverride.ValueMode.IsEmpty())
    {
        TSharedPtr<FJsonObject> Replaced = MakeShared<FJsonObject>();
        Replaced->SetStringField(TEXT("valueMode"), ReplacedOverride.ValueMode);
        if (!ReplacedOverride.Source.IsEmpty())
        {
            Replaced->SetStringField(TEXT("source"), ReplacedOverride.Source);
        }
        if (!ReplacedOverride.Value.IsEmpty())
        {
            // A displaced literal has no source to name; its pin-default text is the only thing
            // that makes the disclosure actionable (it is what a caller would restore).
            Replaced->SetStringField(TEXT("value"), ReplacedOverride.Value);
        }
        Result->SetObjectField(TEXT("replacedOverride"), Replaced);
    }
    Ctx.SendSuccess(Result);
    return true;
}

REGISTER_RPC_HANDLER("niagara.set_module_script", "niagara", "Swap the script asset behind an existing Niagara stack module entry.",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath", "path", "Path to the Niagara System or Niagara Emitter asset"),
        RPC_PARAM_REQ("entryId", "string", "Module entry id or node id, or the owner-qualified entryKey 'owner:id' (a bare id is unique only within one emitter)"),
        RPC_PARAM_REQ("scriptPath", "path", "New UNiagaraScript asset path"),
        RPC_PARAM_OPT("scriptVersion", "string", "Optional version Guid; defaults to script's exposed version"),
        RPC_PARAM_OPT("preserveOverrides", "boolean", "Keep input overrides whose name+type match the new script (default true)"),
        RPC_PARAM_OPT("target", "object", "Module target descriptor"),
        RPC_PARAM_OPT("emitter", "string", "Emitter name for Niagara System assets"),
        RPC_PARAM_OPT("scriptUsage", "string", "Script usage"),
        RPC_PARAM_OPT("compile", "boolean", "Compile after edit"),
        RPC_PARAM_OPT("save", "boolean", "Save after edit")
    ))
{
    FNiagaraModuleEditPayload Payload;
    if (SendNiagaraEditError(Ctx, NiagaraEdit::ParseModulePayload(Ctx.GetRawPayload(), ENiagaraEditOperation::SetModuleScript, Payload))) return true;

    FNiagaraResolvedTarget Target;
    if (SendNiagaraEditError(Ctx, NiagaraEdit::ValidateModulePayload(Payload, ENiagaraEditOperation::SetModuleScript, Target))) return true;

    if (!Target.ModuleNode)
    {
        Ctx.SendError(TEXT("MODULE_NOT_FOUND"), TEXT("Resolved target has no module node."));
        return true;
    }

    const FString PreviousScriptPath = Target.ModuleNode->FunctionScript
        ? Target.ModuleNode->FunctionScript->GetPathName()
        : FString();

    // PreSwapSnapshot is populated inside ApplyModuleMutation (SetModuleScript branch) via OutOverrideSnapshot
    // so we avoid snapshotting twice before and inside the transaction.
    TMap<FNiagaraVariable, FString> PreSwapSnapshot;

    FString NodeId;
    int32 ModuleIndex = INDEX_NONE;
    {
        FScopedTransaction Transaction(FText::FromString(TEXT("MCP: niagara.set_module_script")));
        NiagaraEdit::ModifyResolvedTarget(Target);
        if (Target.Graph)
        {
            Target.Graph->Modify();
        }
        if (SendNiagaraEditError(Ctx, ApplyModuleMutation(Payload, ENiagaraEditOperation::SetModuleScript, Target, false, NodeId, ModuleIndex, &PreSwapSnapshot))) return true;
        NotifyNiagaraGraphChanged(Target);
    }

    // Enumerate new script's inputs for response
    TArray<FNiagaraVariable> NewInputs;
    if (Target.ModuleNode->FunctionScript)
    {
        NiagaraEdit::EnumerateScriptInputs(*Target.ModuleNode->FunctionScript, NewInputs);
    }

    // Build preserved / dropped / addedDefaults lists
    TArray<TSharedPtr<FJsonValue>> PreservedOverrides;
    TArray<TSharedPtr<FJsonValue>> DroppedOverrides;
    TArray<TSharedPtr<FJsonValue>> AddedDefaults;

    for (const TTuple<FNiagaraVariable, FString>& Entry : PreSwapSnapshot)
    {
        const FNiagaraVariable& OldVar = Entry.Key;
        const FNiagaraVariable* Match = NewInputs.FindByPredicate(
            [&OldVar](const FNiagaraVariable& V){ return V.GetName() == OldVar.GetName(); });
        if (!Match)
        {
            TSharedPtr<FJsonObject> Dropped = MakeShared<FJsonObject>();
            Dropped->SetStringField(TEXT("name"), OldVar.GetName().ToString());
            Dropped->SetStringField(TEXT("reason"), TEXT("input_removed"));
            DroppedOverrides.Add(MakeShared<FJsonValueObject>(Dropped));
        }
        else if (Match->GetType() != OldVar.GetType())
        {
            TSharedPtr<FJsonObject> Dropped = MakeShared<FJsonObject>();
            Dropped->SetStringField(TEXT("name"), OldVar.GetName().ToString());
            Dropped->SetStringField(TEXT("reason"), TEXT("type_changed"));
            DroppedOverrides.Add(MakeShared<FJsonValueObject>(Dropped));
        }
        else
        {
            PreservedOverrides.Add(MakeShared<FJsonValueString>(OldVar.GetName().ToString()));
        }
    }

    for (const FNiagaraVariable& NewVar : NewInputs)
    {
        if (!PreSwapSnapshot.Contains(NewVar))
        {
            AddedDefaults.Add(MakeShared<FJsonValueString>(NewVar.GetName().ToString()));
        }
    }

    bool bCompiled = false;
    bool bSaved = false;
    NiagaraEdit::FinalizeNiagaraEdit(Target, Payload.Options, bCompiled, bSaved);

    TSharedPtr<FJsonObject> Result = NiagaraEdit::MakeMutationResult(TEXT("set_module_script"), Target, Payload.Options, bCompiled, bSaved);
    Result->SetBoolField(TEXT("swapped"), true);
    Result->SetStringField(TEXT("previousScript"), PreviousScriptPath);
    Result->SetStringField(TEXT("entryId"), Payload.Target.EntryId);
    Result->SetStringField(TEXT("nodeId"), NodeId);
    Result->SetArrayField(TEXT("preservedOverrides"), PreservedOverrides);
    Result->SetArrayField(TEXT("droppedOverrides"), DroppedOverrides);
    Result->SetArrayField(TEXT("addedDefaults"), AddedDefaults);
    Ctx.SendSuccess(Result);
    return true;
}

REGISTER_RPC_HANDLER("niagara.set_static_switch", "niagara", "Set the resolved value of a static switch input on a stack module.",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath", "path", "Path to the Niagara System or Niagara Emitter asset"),
        RPC_PARAM_REQ("entryId", "string", "Module entry id (function-call node id), or the owner-qualified entryKey 'owner:id' (a bare id is unique only within one emitter)"),
        RPC_PARAM_REQ("inputName", "string", "Static switch input name"),
        RPC_PARAM_REQ("value", "any", "New resolved value: bool, an in-range int, or for an enum switch the branch index or the entry/display name shown in the editor. On Integer switches bool maps to 1/0"),
        RPC_PARAM_OPT("target", "object", "Module target descriptor"),
        RPC_PARAM_OPT("emitter", "string", "Emitter name for Niagara System assets"),
        RPC_PARAM_OPT("scriptUsage", "string", "Script usage"),
        RPC_PARAM_OPT("compile", "boolean", "Compile after edit"),
        RPC_PARAM_OPT("save", "boolean", "Save after edit")
    ))
{
    FNiagaraModuleEditPayload Payload;
    if (SendNiagaraEditError(Ctx, NiagaraEdit::ParseModulePayload(Ctx.GetRawPayload(), ENiagaraEditOperation::SetModuleInput, Payload))) return true;

    FNiagaraResolvedTarget Target;
    if (SendNiagaraEditError(Ctx, NiagaraEdit::ValidateModulePayload(Payload, ENiagaraEditOperation::SetModuleInput, Target))) return true;

    if (!Target.ModuleNode)
    {
        Ctx.SendError(TEXT("MODULE_NOT_FOUND"), TEXT("Resolved target has no module node."));
        return true;
    }

    UNiagaraGraph* CalledGraph = Target.ModuleNode->GetCalledGraph();
    if (!CalledGraph)
    {
        Ctx.SendError(TEXT("STATIC_SWITCH_NOT_FOUND"), TEXT("Module has no called graph."));
        return true;
    }

    const FName InputFName(*Payload.InputName);
    const UNiagaraNodeStaticSwitch* SwitchDecl = nullptr;
    if (!NiagaraStaticSwitch::FindByName(CalledGraph, InputFName, SwitchDecl))
    {
        Ctx.SendError(TEXT("STATIC_SWITCH_NOT_FOUND"),
            FString::Printf(TEXT("No static switch named '%s' in called graph."), *Payload.InputName));
        return true;
    }

    const UEnum* SwitchEnum = SwitchDecl->SwitchTypeData.Enum;
    const bool bIntegerSwitch = SwitchDecl->SwitchTypeData.SwitchType == ENiagaraStaticSwitchType::Integer;
    const bool bEnumSwitch = SwitchDecl->SwitchTypeData.SwitchType == ENiagaraStaticSwitchType::Enum && SwitchEnum != nullptr;

    FString PinDefaultValue;
    FString EncodeError;
    NiagaraStaticSwitch::FEnumSwitchOption ChosenOption;
    if (!NiagaraStaticSwitch::EncodePinDefault(Payload.Value, SwitchDecl->SwitchTypeData.SwitchType, SwitchEnum, PinDefaultValue, EncodeError, &ChosenOption))
    {
        // Publish the branch table on the rejection too. A caller who guessed the wrong integer or
        // typed a label this enum does not carry needs the index <-> label mapping to retry, and
        // no other read of a rejected call carries it.
        TSharedPtr<FJsonObject> ErrorData;
        if (bEnumSwitch)
        {
            ErrorData = MakeShared<FJsonObject>();
            ErrorData->SetStringField(TEXT("enumPath"), SwitchEnum->GetPathName());
            ErrorData->SetArrayField(TEXT("enumOptions"), NiagaraStaticSwitch::MakeEnumOptionsJson(SwitchEnum));
        }
        Ctx.SendError(TEXT("INVALID_VALUE"), EncodeError, ErrorData);
        return true;
    }
    if (bIntegerSwitch
        && !NiagaraStaticSwitch::ValidateIntegerOption(
            *SwitchDecl,
            FCString::Atoi(*PinDefaultValue),
            EncodeError))
    {
        Ctx.SendError(TEXT("INVALID_VALUE"), EncodeError);
        return true;
    }

    UEdGraphPin* CallerPin = nullptr;
    {
        FScopedTransaction Transaction(FText::FromString(TEXT("MCP: niagara.set_static_switch")));
        NiagaraEdit::ModifyResolvedTarget(Target);
        if (Target.Graph)
        {
            Target.Graph->Modify();
        }
        Target.ModuleNode->Modify();

        // Reimplement UNiagaraNodeFunctionCall::FindStaticSwitchInputPin — that method exists in
        // NiagaraEditor's UCLASS(MinimalAPI) class without NIAGARAEDITOR_API decoration. Since
        // FindByName above already confirmed the switch is declared in the called graph, finding
        // the caller pin reduces to matching by name on the module node's input pins.
        for (UEdGraphPin* Pin : Target.ModuleNode->Pins)
        {
            if (Pin && Pin->Direction == EGPD_Input && Pin->GetFName() == InputFName)
            {
                CallerPin = Pin;
                break;
            }
        }
        if (!CallerPin)
        {
            Ctx.SendError(TEXT("STATIC_SWITCH_NOT_FOUND"),
                FString::Printf(TEXT("Module has no static switch caller pin named '%s'."), *Payload.InputName));
            return true;
        }

        CallerPin->Modify();
        CallerPin->DefaultValue = PinDefaultValue;
        NiagaraEdit::NotifyNiagaraObjectChanged(Target, nullptr);
    }

    bool bCompiled = false;
    bool bSaved = false;
    NiagaraEdit::FinalizeNiagaraEdit(Target, Payload.Options, bCompiled, bSaved);

    TSharedPtr<FJsonObject> Result = NiagaraEdit::MakeMutationResult(TEXT("set_static_switch"), Target, Payload.Options, bCompiled, bSaved);
    Result->SetStringField(TEXT("entryId"), Payload.Target.EntryId);
    Result->SetStringField(TEXT("inputName"), Payload.InputName);
    Result->SetStringField(TEXT("value"), PinDefaultValue);
    if (bEnumSwitch)
    {
        // `value` is the authored entry name the pin now holds, which on a user-defined enum is a
        // NewEnumeratorN that means nothing on its own. Echo the branch index and the editor's
        // label beside it, and the whole table, so the write can be read back without probing.
        Result->SetNumberField(TEXT("index"), ChosenOption.Index);
        Result->SetStringField(TEXT("displayName"), ChosenOption.DisplayName);
        Result->SetStringField(TEXT("enumPath"), SwitchEnum->GetPathName());
        Result->SetArrayField(TEXT("enumOptions"), NiagaraStaticSwitch::MakeEnumOptionsJson(SwitchEnum));
    }
    Ctx.SendSuccess(Result);
    return true;
}

REGISTER_RPC_HANDLER("niagara.reset_module_input", "niagara",
    "Reset one Niagara module input override back to its default. For override-pin inputs removes the pin and chained upstream nodes; for static-switch inputs resets the caller-pin DefaultValue.",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath", "path", "Path to the Niagara System or Niagara Emitter asset"),
        RPC_PARAM_REQ("entryId", "string", "Module entry id (function-call node id), or the owner-qualified entryKey 'owner:id' (a bare id is unique only within one emitter)"),
        RPC_PARAM_REQ("inputName", "string", "Module input name to reset"),
        RPC_PARAM_OPT("target", "object", "Module target descriptor"),
        RPC_PARAM_OPT("emitter", "string", "Emitter name for Niagara System assets"),
        RPC_PARAM_OPT("scriptUsage", "string", "Script usage"),
        RPC_PARAM_OPT("compile", "boolean", "Compile after edit"),
        RPC_PARAM_OPT("save", "boolean", "Save after edit")
    ))
{
    FNiagaraModuleEditPayload Payload;
    if (SendNiagaraEditError(Ctx, NiagaraEdit::ParseModulePayload(Ctx.GetRawPayload(), ENiagaraEditOperation::ResetModuleInput, Payload))) return true;

    FNiagaraResolvedTarget Target;
    if (SendNiagaraEditError(Ctx, NiagaraEdit::ValidateModulePayload(Payload, ENiagaraEditOperation::ResetModuleInput, Target))) return true;

    if (!Target.ModuleNode)
    {
        Ctx.SendError(TEXT("MODULE_NOT_FOUND"), TEXT("Resolved target has no module node."));
        return true;
    }

    const FName InputFName(*Payload.InputName);

    // --- Static-switch path ---
    UNiagaraGraph* CalledGraph = Target.ModuleNode->GetCalledGraph();
    const UNiagaraNodeStaticSwitch* SwitchDecl = nullptr;
    if (CalledGraph && NiagaraStaticSwitch::FindByName(CalledGraph, InputFName, SwitchDecl))
    {
        // Find caller pin on the module node
        UEdGraphPin* CallerPin = nullptr;
        for (UEdGraphPin* Pin : Target.ModuleNode->Pins)
        {
            if (Pin && Pin->Direction == EGPD_Input && Pin->GetFName() == InputFName)
            {
                CallerPin = Pin;
                break;
            }
        }
        if (!CallerPin)
        {
            Ctx.SendError(TEXT("INPUT_NOT_FOUND"),
                FString::Printf(TEXT("No caller pin named '%s' on module node."), *Payload.InputName));
            return true;
        }

        const FString PreviousValue = CallerPin->DefaultValue;

        {
            FScopedTransaction Transaction(FText::FromString(TEXT("MCP: niagara.reset_module_input (switch)")));
            NiagaraEdit::ModifyResolvedTarget(Target);
            if (Target.Graph)
            {
                Target.Graph->Modify();
            }
            Target.ModuleNode->Modify();
            CallerPin->Modify();
            GetDefault<UEdGraphSchema_Niagara>()->ResetPinToAutogeneratedDefaultValue(CallerPin, true);
            NiagaraEdit::NotifyNiagaraObjectChanged(Target, nullptr);
        }

        bool bCompiled = false;
        bool bSaved = false;
        NiagaraEdit::FinalizeNiagaraEdit(Target, Payload.Options, bCompiled, bSaved);

        TSharedPtr<FJsonObject> Result = NiagaraEdit::MakeMutationResult(TEXT("reset_module_input"), Target, Payload.Options, bCompiled, bSaved);
        Result->SetBoolField(TEXT("reset"), true);
        Result->SetStringField(TEXT("previousValue"), PreviousValue);
        Result->SetStringField(TEXT("kind"), TEXT("switch"));
        Result->SetStringField(TEXT("inputName"), Payload.InputName);
        Ctx.SendSuccess(Result);
        return true;
    }

    // --- Override-pin path ---
    UNiagaraNodeParameterMapSet* OverrideNode = NiagaraResetModuleInput::FindStackFunctionOverrideNode(*Target.ModuleNode);
    if (!OverrideNode)
    {
        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetBoolField(TEXT("reset"), false);
        Result->SetStringField(TEXT("kind"), TEXT("rapid"));
        Result->SetStringField(TEXT("inputName"), Payload.InputName);
        Result->SetStringField(TEXT("reason"), TEXT("no_override_node"));
        Ctx.SendSuccess(Result);
        return true;
    }

    // Build the aliased parameter handle name to find the pin
    const FNiagaraParameterHandle InputHandle = Payload.InputName.Contains(TEXT("."))
        ? FNiagaraParameterHandle(FName(*Payload.InputName))
        : FNiagaraParameterHandle::CreateModuleParameterHandle(FName(*Payload.InputName));
    const FNiagaraParameterHandle AliasedInputHandle =
        FNiagaraParameterHandle::CreateAliasedModuleParameterHandle(InputHandle, Target.ModuleNode);
    const FName AliasedName = AliasedInputHandle.GetParameterHandleString();

    TArray<UEdGraphPin*> OverridePins;
    OverrideNode->GetInputPins(OverridePins);
    UEdGraphPin* TargetPin = nullptr;
    for (UEdGraphPin* Pin : OverridePins)
    {
        if (Pin && Pin->PinName == AliasedName)
        {
            TargetPin = Pin;
            break;
        }
    }

    if (!TargetPin)
    {
        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetBoolField(TEXT("reset"), false);
        Result->SetStringField(TEXT("kind"), TEXT("rapid"));
        Result->SetStringField(TEXT("inputName"), Payload.InputName);
        Result->SetStringField(TEXT("reason"), TEXT("pin_not_found"));
        Ctx.SendSuccess(Result);
        return true;
    }

    const FString PreviousValue = TargetPin->DefaultValue;
    const bool bWasLinked = TargetPin->LinkedTo.Num() > 0;

    if (Target.System)
    {
        Target.QuiescedInstances += PinWrightNiagara::KillSystemInstances(*Target.System);
    }

    // Removing the override pin is only half a reset once set_module_input writes the input's
    // rapid-iteration constant too: leaving the constant standing would revert the graph while the
    // runtime kept the last written value — the same defect inverted. Resolved before the
    // transaction because the module's owning stage decides whether the constant carries an emitter
    // segment, and the walk needs the graph intact.
    const UNiagaraNodeOutput* OwningOutputNode = FindOwningStackOutputNode(Target);
    const FName ResetConstantName = OwningOutputNode
        ? PinWrightNiagara::MakeRapidIterationConstantName(
            AliasedName,
            Target.Emitter ? Target.Emitter->GetUniqueEmitterName() : FString(),
            OwningOutputNode->GetUsage())
        : NAME_None;
    PinWrightNiagara::FRapidIterationWriteThrough RapidIteration;

    {
        FScopedTransaction Transaction(FText::FromString(TEXT("MCP: niagara.reset_module_input (rapid)")));
        NiagaraEdit::ModifyResolvedTarget(Target);
        if (Target.Graph)
        {
            Target.Graph->Modify();
        }
        TArray<TWeakObjectPtr<UNiagaraDataInterface>> RemovedDI;
        NiagaraResetModuleInput::RemoveOverridePinAndChainedNodes(*TargetPin, *Target.Graph, RemovedDI);
        if (!ResetConstantName.IsNone())
        {
            PinWrightNiagara::RemoveModuleInputConstant(Target.System, Target.Emitter, ResetConstantName, RapidIteration);
        }
        NiagaraEdit::NotifyNiagaraObjectChanged(Target, nullptr);
        // RemoveOverridePinAndChainedNodes MarkAsGarbage()s the override UEdGraphPin, and
        // UEdGraphNode::RemovePin sends no graph notification of its own: it reaches
        // UNiagaraNode::OnPinRemoved -> MarkNodeRequiresSynchronization -> NotifyGraphNeedsRecompile,
        // and UNiagaraGraph::NotifyGraphChanged early-returns on GRAPHACTION_GenericNeedsRecompile
        // without ever broadcasting OnGraphChanged. An open toolkit therefore keeps that freed pin
        // in UNiagaraStackFunctionInput::OverridePinCache (a bare UEdGraphPin*) and in SGraphPin's
        // GraphPinObj. Every sibling graph mutator in this file already ends with
        // NotifyNiagaraGraphChanged for exactly this reason; this one and clear_module_overrides
        // were the two that did not. The broadcast lands inside the transaction, before any Slate
        // tick, so the caches are dropped before anything can read them.
        NotifyNiagaraGraphChanged(Target);
    }

    bool bCompiled = false;
    bool bSaved = false;
    NiagaraEdit::FinalizeNiagaraEdit(Target, Payload.Options, bCompiled, bSaved);

    TSharedPtr<FJsonObject> Result = NiagaraEdit::MakeMutationResult(TEXT("reset_module_input"), Target, Payload.Options, bCompiled, bSaved);
    Result->SetBoolField(TEXT("reset"), true);
    Result->SetStringField(TEXT("previousValue"), PreviousValue);
    Result->SetBoolField(TEXT("wasLinked"), bWasLinked);
    Result->SetStringField(TEXT("kind"), TEXT("rapid"));
    Result->SetStringField(TEXT("inputName"), Payload.InputName);
    if (TSharedPtr<FJsonObject> Constant = MakeRapidIterationReport(RapidIteration))
    {
        Result->SetObjectField(TEXT("rapidIteration"), Constant);
    }
    Ctx.SendSuccess(Result);
    return true;
}

REGISTER_RPC_HANDLER("niagara.clear_module_overrides", "niagara",
    "Remove all override-pin inputs and reset all non-default static-switch caller pins for a given stack module.",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath", "path", "Path to the Niagara System or Niagara Emitter asset"),
        RPC_PARAM_REQ("entryId", "string", "Module entry id (function-call node id), or the owner-qualified entryKey 'owner:id' (a bare id is unique only within one emitter)"),
        RPC_PARAM_OPT("target", "object", "Module target descriptor"),
        RPC_PARAM_OPT("emitter", "string", "Emitter name for Niagara System assets"),
        RPC_PARAM_OPT("scriptUsage", "string", "Script usage"),
        RPC_PARAM_OPT("compile", "boolean", "Compile after edit"),
        RPC_PARAM_OPT("save", "boolean", "Save after edit")
    ))
{
    FNiagaraModuleEditPayload Payload;
    if (SendNiagaraEditError(Ctx, NiagaraEdit::ParseModulePayload(Ctx.GetRawPayload(), ENiagaraEditOperation::ClearModuleOverrides, Payload))) return true;

    FNiagaraResolvedTarget Target;
    if (SendNiagaraEditError(Ctx, NiagaraEdit::ValidateModulePayload(Payload, ENiagaraEditOperation::ClearModuleOverrides, Target))) return true;

    if (!Target.ModuleNode)
    {
        Ctx.SendError(TEXT("MODULE_NOT_FOUND"), TEXT("Resolved target has no module node."));
        return true;
    }

    if (Target.System)
    {
        Target.QuiescedInstances += PinWrightNiagara::KillSystemInstances(*Target.System);
    }

    TArray<TSharedPtr<FJsonValue>> ClearedNames;
    // Same reasoning as reset_module_input's override branch: a cleared pin whose rapid-iteration
    // constant survives leaves the runtime on the last written value while the graph reads the
    // module default. Resolved before the transaction, while the stack walk still has its graph.
    const UNiagaraNodeOutput* OwningOutputNode = FindOwningStackOutputNode(Target);
    const FString UniqueEmitterName = Target.Emitter ? Target.Emitter->GetUniqueEmitterName() : FString();
    TArray<TSharedPtr<FJsonValue>> ClearedConstants;

    {
        FScopedTransaction Transaction(FText::FromString(TEXT("MCP: niagara.clear_module_overrides")));
        NiagaraEdit::ModifyResolvedTarget(Target);
        if (Target.Graph)
        {
            Target.Graph->Modify();
        }

        // --- Override-pin inputs ---
        UNiagaraNodeParameterMapSet* OverrideNode = NiagaraResetModuleInput::FindStackFunctionOverrideNode(*Target.ModuleNode);
        if (OverrideNode)
        {
            const FName ModuleFunctionName = FName(*Target.ModuleNode->GetFunctionName());

            TArray<UEdGraphPin*> OverridePins;
            OverrideNode->GetInputPins(OverridePins);

            // Collect pins belonging to this module (filter by namespace == module function name)
            TArray<UEdGraphPin*> ModulePins;
            for (UEdGraphPin* Pin : OverridePins)
            {
                if (!Pin)
                {
                    continue;
                }
                const FNiagaraParameterHandle Handle(Pin->PinName);
                if (Handle.GetNamespace() == ModuleFunctionName)
                {
                    ModulePins.Add(Pin);
                }
            }

            // Remove each pin (must collect first; removing invalidates array)
            for (UEdGraphPin* Pin : ModulePins)
            {
                const FName AliasedPinName = Pin->PinName;
                ClearedNames.Add(MakeShared<FJsonValueString>(AliasedPinName.ToString()));
                TArray<TWeakObjectPtr<UNiagaraDataInterface>> RemovedDI;
                NiagaraResetModuleInput::RemoveOverridePinAndChainedNodes(*Pin, *Target.Graph, RemovedDI);
                if (!OwningOutputNode)
                {
                    continue;
                }
                PinWrightNiagara::FRapidIterationWriteThrough RapidIteration;
                PinWrightNiagara::RemoveModuleInputConstant(
                    Target.System,
                    Target.Emitter,
                    PinWrightNiagara::MakeRapidIterationConstantName(
                        AliasedPinName, UniqueEmitterName, OwningOutputNode->GetUsage()),
                    RapidIteration);
                // Only the inputs that actually had a constant are listed: on a module whose
                // inputs are all pin-driven this array is empty, which is the true answer.
                if (RapidIteration.bShadowed)
                {
                    if (TSharedPtr<FJsonObject> Constant = MakeRapidIterationReport(RapidIteration))
                    {
                        ClearedConstants.Add(MakeShared<FJsonValueObject>(Constant));
                    }
                }
            }
        }

        // --- Static-switch caller pins ---
        UNiagaraGraph* CalledGraph = Target.ModuleNode->GetCalledGraph();
        if (CalledGraph)
        {
            for (UEdGraphPin* Pin : Target.ModuleNode->Pins)
            {
                if (!Pin || Pin->Direction != EGPD_Input)
                {
                    continue;
                }
                const UNiagaraNodeStaticSwitch* SwitchDecl = nullptr;
                if (!NiagaraStaticSwitch::FindByName(CalledGraph, Pin->GetFName(), SwitchDecl))
                {
                    continue;
                }
                // Reset to autogenerated default; detect change by comparing before/after.
                const FString PreviousPinDefault = Pin->DefaultValue;
                const FString PinNameStr = Pin->PinName.ToString();
                Pin->Modify();
                GetDefault<UEdGraphSchema_Niagara>()->ResetPinToAutogeneratedDefaultValue(Pin, true);
                if (Pin->DefaultValue != PreviousPinDefault)
                {
                    ClearedNames.Add(MakeShared<FJsonValueString>(PinNameStr));
                }
            }
        }

        NiagaraEdit::NotifyNiagaraObjectChanged(Target, nullptr);
        // Same freed-override-pin exposure as reset_module_input's override branch; see the comment
        // there. Broadcast OnGraphChanged so an open toolkit drops the UEdGraphPin pointers this
        // teardown just MarkAsGarbage()d.
        NotifyNiagaraGraphChanged(Target);
    }

    bool bCompiled = false;
    bool bSaved = false;
    NiagaraEdit::FinalizeNiagaraEdit(Target, Payload.Options, bCompiled, bSaved);

    TSharedPtr<FJsonObject> Result = NiagaraEdit::MakeMutationResult(TEXT("clear_module_overrides"), Target, Payload.Options, bCompiled, bSaved);
    Result->SetNumberField(TEXT("cleared"), ClearedNames.Num());
    Result->SetArrayField(TEXT("inputs"), ClearedNames);
    // One entry per cleared input that carried a rapid-iteration constant, in the same shape
    // set_module_input / reset_module_input report as their single `rapidIteration` object.
    Result->SetArrayField(TEXT("rapidIteration"), ClearedConstants);
    Result->SetStringField(TEXT("entryId"), Payload.Target.EntryId);
    Ctx.SendSuccess(Result);
    return true;
}

REGISTER_RPC_HANDLER("niagara.set_scalability_property", "niagara",
    "Set a scalability override field on the system or a per-quality emitter override.",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath", "path", "Path to the Niagara System or Niagara Emitter asset"),
        RPC_PARAM_REQ("target", "object", "Descriptor: {kind:'system'|'emitter', emitter?:string, qualityLevel?:int}"),
        RPC_PARAM_REQ("propertyPath", "string", "Field on the override struct (e.g. 'MaxDistance', 'bOverrideDistanceSettings')"),
        RPC_PARAM_REQ("value", "any", "Value to assign"),
        RPC_PARAM_OPT("compile", "boolean", "Compile after edit"),
        RPC_PARAM_OPT("save", "boolean", "Save after edit")
    ))
{
    FNiagaraPropertyEditPayload Payload;
    if (SendNiagaraEditError(Ctx, NiagaraEdit::ParsePropertyPayload(Ctx.GetRawPayload(), Payload))) return true;

    // qualityLevel is specific to scalability — read it from the target descriptor on the raw payload
    // rather than extending FNiagaraEditTargetSpec for a single consumer.
    int32 QualityLevel = 0;
    if (TSharedPtr<FJsonObject> TargetObj = Ctx.GetObject(TEXT("target")))
    {
        double QlNumber = 0.0;
        if (TargetObj->TryGetNumberField(TEXT("qualityLevel"), QlNumber))
        {
            QualityLevel = static_cast<int32>(QlNumber);
        }
    }
    // Clamp to engine-known max so a hostile caller cannot OOM us by passing INT32_MAX.
    const int32 EngineMaxLevel = FNiagaraPlatformSet::GetMaxQualityLevel();
    const int32 MaxLevels = EngineMaxLevel > 0 ? EngineMaxLevel : 8;
    if (QualityLevel < 0 || QualityLevel > MaxLevels)
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"),
            FString::Printf(TEXT("'target.qualityLevel' must be in [0, %d]."), MaxLevels));
        return true;
    }

    // Resolve target via existing helper to validate asset/emitter; we discard the reflected
    // property path since scalability override structs aren't exposed as direct UPROPERTYs on the target.
    FNiagaraResolvedTarget Target;
    if (FNiagaraEditError ResolveError = NiagaraEdit::ResolveTarget(Payload.AssetPath, Payload.Target, Target); ResolveError.HasError())
    {
        Ctx.SendError(ResolveError.Code, ResolveError.Message);
        return true;
    }

    const ENiagaraEditTargetKind Kind = Payload.Target.Kind;
    const bool bIsSystem = Kind == ENiagaraEditTargetKind::System;
    const bool bIsEmitter = Kind == ENiagaraEditTargetKind::EmitterHandle || Kind == ENiagaraEditTargetKind::EmitterData;
    if (!bIsSystem && !bIsEmitter)
    {
        Ctx.SendError(TEXT("UNSUPPORTED_TARGET"),
            FString::Printf(TEXT("Target kind '%s' has no scalability overrides."), *NiagaraEdit::TargetKindToString(Kind)));
        return true;
    }

    void* Container = nullptr;
    UScriptStruct* StructDef = nullptr;
    {
        FScopedTransaction Transaction(FText::FromString(TEXT("MCP: niagara.set_scalability_property")));
        NiagaraEdit::ModifyResolvedTarget(Target);

        auto EnsureOverrideSlot = [QualityLevel](auto& OverrideArray)
        {
            while (OverrideArray.Num() <= QualityLevel)
            {
                OverrideArray.AddDefaulted();
            }
        };

        if (bIsSystem)
        {
            if (!Target.System)
            {
                Ctx.SendError(TEXT("RESOLUTION_FAILED"), TEXT("System target resolved with null UNiagaraSystem."));
                return true;
            }
            FNiagaraSystemScalabilityOverrides& Overrides = Target.System->GetScalabilityOverrides();
            EnsureOverrideSlot(Overrides.Overrides);
            Container = &Overrides.Overrides[QualityLevel];
            StructDef = FNiagaraSystemScalabilityOverride::StaticStruct();
        }
        else
        {
            if (!Target.EmitterData)
            {
                Ctx.SendError(TEXT("RESOLUTION_FAILED"), TEXT("Emitter target resolved with null FVersionedNiagaraEmitterData."));
                return true;
            }
            FNiagaraEmitterScalabilityOverrides& Overrides = Target.EmitterData->ScalabilityOverrides;
            EnsureOverrideSlot(Overrides.Overrides);
            Container = &Overrides.Overrides[QualityLevel];
            StructDef = FNiagaraEmitterScalabilityOverride::StaticStruct();
        }

        FProperty* Property = FindFProperty<FProperty>(StructDef, FName(*Payload.PropertyPath));
        if (!Property)
        {
            Ctx.SendError(TEXT("PROPERTY_NOT_FOUND"),
                FString::Printf(TEXT("No property '%s' on '%s'."), *Payload.PropertyPath, *StructDef->GetName()));
            return true;
        }

        FString ApplyError;
        if (!ApplyJsonValueToProperty(Container, Property, Payload.Value, ApplyError))
        {
            Ctx.SendError(TEXT("PROPERTY_SET_FAILED"),
                FString::Printf(TEXT("Failed to set '%s': %s"), *Payload.PropertyPath, *ApplyError));
            return true;
        }

        // UpdateScalability is the engine's own re-cache hook on UNiagaraSystem (NIAGARA_API).
        // The matching method on UNiagaraEmitter is not exported, so emitter-only paths rely on
        // PostEditChange + InvalidateCompileResults inside NotifyNiagaraObjectChanged.
        if (Target.System)
        {
            Target.System->UpdateScalability();
        }
        NiagaraEdit::NotifyNiagaraObjectChanged(Target, Property);
    }

    bool bCompiled = false;
    bool bSaved = false;
    NiagaraEdit::FinalizeNiagaraEdit(Target, Payload.Options, bCompiled, bSaved);

    TSharedPtr<FJsonObject> Result = NiagaraEdit::MakeMutationResult(TEXT("set_scalability_property"), Target, Payload.Options, bCompiled, bSaved);
    Result->SetStringField(TEXT("targetKind"), bIsSystem ? TEXT("system") : TEXT("emitter"));
    Result->SetNumberField(TEXT("qualityLevel"), QualityLevel);
    Result->SetStringField(TEXT("propertyPath"), Payload.PropertyPath);
    Ctx.SendSuccess(Result);
    return true;
}

REGISTER_RPC_HANDLER("niagara.connect_pin", "niagara", "Connect one Niagara graph pin pair.",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath", "path", "Path to the Niagara System or Niagara Emitter asset"),
        RPC_PARAM_REQ("fromNode", "string", "Source node id/name"),
        RPC_PARAM_REQ("fromPin", "string", "Source pin name/id"),
        RPC_PARAM_REQ("toNode", "string", "Destination node id/name"),
        RPC_PARAM_REQ("toPin", "string", "Destination pin name/id"),
        RPC_PARAM_OPT("target", "object", "Graph target descriptor"),
        RPC_PARAM_OPT("emitter", "string", "Emitter name for Niagara System assets"),
        RPC_PARAM_OPT("scriptUsage", "string", "Script usage"),
        RPC_PARAM_OPT("compile", "boolean", "Compile after edit"),
        RPC_PARAM_OPT("save", "boolean", "Save after edit")
    ))
{
    FNiagaraPinEditPayload Payload;
    if (SendNiagaraEditError(Ctx, NiagaraEdit::ParsePinPayload(Ctx.GetRawPayload(), ENiagaraEditOperation::ConnectPin, Payload))) return true;

    FNiagaraResolvedTarget Target;
    if (SendNiagaraEditError(Ctx, NiagaraEdit::ValidatePinPayload(Payload, ENiagaraEditOperation::ConnectPin, Target))) return true;

    FString DefaultValue;
    {
        FScopedTransaction Transaction(FText::FromString(TEXT("MCP: niagara.connect_pin")));
        NiagaraEdit::ModifyResolvedTarget(Target);
        if (Target.Graph)
        {
            Target.Graph->Modify();
        }
        if (SendNiagaraEditError(Ctx, ApplyPinMutation(Payload, ENiagaraEditOperation::ConnectPin, Target, DefaultValue))) return true;
        NotifyNiagaraGraphChanged(Target);
    }

    bool bCompiled = false;
    bool bSaved = false;
    NiagaraEdit::FinalizeNiagaraEdit(Target, Payload.Options, bCompiled, bSaved);

    TSharedPtr<FJsonObject> Result = NiagaraEdit::MakeMutationResult(TEXT("connect_pin"), Target, Payload.Options, bCompiled, bSaved);
    Result->SetStringField(TEXT("fromNode"), Payload.FromNode);
    Result->SetStringField(TEXT("fromPin"), Payload.FromPin);
    Result->SetStringField(TEXT("toNode"), Payload.ToNode);
    Result->SetStringField(TEXT("toPin"), Payload.ToPin);
    Ctx.SendSuccess(Result);
    return true;
}

REGISTER_RPC_HANDLER("niagara.disconnect_pin", "niagara", "Disconnect one Niagara graph pin pair.",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath", "path", "Path to the Niagara System or Niagara Emitter asset"),
        RPC_PARAM_REQ("fromNode", "string", "Source node id/name"),
        RPC_PARAM_REQ("fromPin", "string", "Source pin name/id"),
        RPC_PARAM_REQ("toNode", "string", "Destination node id/name"),
        RPC_PARAM_REQ("toPin", "string", "Destination pin name/id"),
        RPC_PARAM_OPT("target", "object", "Graph target descriptor"),
        RPC_PARAM_OPT("emitter", "string", "Emitter name for Niagara System assets"),
        RPC_PARAM_OPT("scriptUsage", "string", "Script usage"),
        RPC_PARAM_OPT("compile", "boolean", "Compile after edit"),
        RPC_PARAM_OPT("save", "boolean", "Save after edit")
    ))
{
    FNiagaraPinEditPayload Payload;
    if (SendNiagaraEditError(Ctx, NiagaraEdit::ParsePinPayload(Ctx.GetRawPayload(), ENiagaraEditOperation::DisconnectPin, Payload))) return true;

    FNiagaraResolvedTarget Target;
    if (SendNiagaraEditError(Ctx, NiagaraEdit::ValidatePinPayload(Payload, ENiagaraEditOperation::DisconnectPin, Target))) return true;

    FString DefaultValue;
    {
        FScopedTransaction Transaction(FText::FromString(TEXT("MCP: niagara.disconnect_pin")));
        NiagaraEdit::ModifyResolvedTarget(Target);
        if (Target.Graph)
        {
            Target.Graph->Modify();
        }
        if (SendNiagaraEditError(Ctx, ApplyPinMutation(Payload, ENiagaraEditOperation::DisconnectPin, Target, DefaultValue))) return true;
        NotifyNiagaraGraphChanged(Target);
    }

    bool bCompiled = false;
    bool bSaved = false;
    NiagaraEdit::FinalizeNiagaraEdit(Target, Payload.Options, bCompiled, bSaved);

    TSharedPtr<FJsonObject> Result = NiagaraEdit::MakeMutationResult(TEXT("disconnect_pin"), Target, Payload.Options, bCompiled, bSaved);
    Result->SetStringField(TEXT("fromNode"), Payload.FromNode);
    Result->SetStringField(TEXT("fromPin"), Payload.FromPin);
    Result->SetStringField(TEXT("toNode"), Payload.ToNode);
    Result->SetStringField(TEXT("toPin"), Payload.ToPin);
    Ctx.SendSuccess(Result);
    return true;
}

REGISTER_RPC_HANDLER("niagara.set_pin_default", "niagara", "Set one Niagara graph pin default value.",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath", "path", "Path to the Niagara System or Niagara Emitter asset"),
        RPC_PARAM_REQ("nodeId", "string", "Node id/name"),
        RPC_PARAM_REQ("pin", "string", "Pin name/id"),
        RPC_PARAM_REQ("defaultValue", "any", "Default value"),
        RPC_PARAM_OPT("target", "object", "Graph target descriptor"),
        RPC_PARAM_OPT("emitter", "string", "Emitter name for Niagara System assets"),
        RPC_PARAM_OPT("scriptUsage", "string", "Script usage"),
        RPC_PARAM_OPT("compile", "boolean", "Compile after edit"),
        RPC_PARAM_OPT("save", "boolean", "Save after edit")
    ))
{
    FNiagaraPinEditPayload Payload;
    if (SendNiagaraEditError(Ctx, NiagaraEdit::ParsePinPayload(Ctx.GetRawPayload(), ENiagaraEditOperation::SetPinDefault, Payload))) return true;

    FNiagaraResolvedTarget Target;
    if (SendNiagaraEditError(Ctx, NiagaraEdit::ValidatePinPayload(Payload, ENiagaraEditOperation::SetPinDefault, Target))) return true;

    FString DefaultValue;
    {
        FScopedTransaction Transaction(FText::FromString(TEXT("MCP: niagara.set_pin_default")));
        NiagaraEdit::ModifyResolvedTarget(Target);
        if (Target.Graph)
        {
            Target.Graph->Modify();
        }
        if (SendNiagaraEditError(Ctx, ApplyPinMutation(Payload, ENiagaraEditOperation::SetPinDefault, Target, DefaultValue))) return true;
        NotifyNiagaraGraphChanged(Target);
    }

    bool bCompiled = false;
    bool bSaved = false;
    NiagaraEdit::FinalizeNiagaraEdit(Target, Payload.Options, bCompiled, bSaved);

    TSharedPtr<FJsonObject> Result = NiagaraEdit::MakeMutationResult(TEXT("set_pin_default"), Target, Payload.Options, bCompiled, bSaved);
    Result->SetStringField(TEXT("nodeId"), Payload.NodeId);
    Result->SetStringField(TEXT("pin"), Payload.PinName);
    Result->SetStringField(TEXT("defaultValue"), DefaultValue);
    Ctx.SendSuccess(Result);
    return true;
}
