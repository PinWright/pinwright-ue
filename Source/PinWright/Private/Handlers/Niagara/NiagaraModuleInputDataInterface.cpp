// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/Niagara/NiagaraModuleInputDataInterface.h"

#include "Handlers/Niagara/NiagaraJsonHelpers.h"
#include "Handlers/Niagara/NiagaraResetModuleInputHelpers.h"

#include "EdGraph/EdGraphPin.h"
#include "NiagaraDataInterface.h"
#include "NiagaraGraph.h"
#include "NiagaraNodeFunctionCall.h"
#include "NiagaraNodeInput.h"
#include "NiagaraNodeParameterMapSet.h"
#include "NiagaraScript.h"
#include "UObject/UnrealType.h"
#include "ViewModels/Stack/NiagaraParameterHandle.h"
#include "ViewModels/Stack/NiagaraStackGraphUtilities.h"

namespace NiagaraModuleInputDI
{
namespace
{
    // EnumerateModuleStackInputs returns "Module.<Name>" variables; the wire spelling a caller
    // uses for `inputName` is the trailing half, matching niagara.set_module_input.
    FString ShortStackInputName(const FNiagaraVariable& Variable)
    {
        return FNiagaraParameterHandle(Variable.GetName()).GetName().ToString();
    }

    // Find the declared stack input matching InputName (short or fully namespaced spelling,
    // case-insensitive) and, either way, collect the module's data-interface-typed input names so
    // a miss can name the spellings that would have worked.
    bool FindDeclaredStackInput(
        const UNiagaraNodeFunctionCall& ModuleNode,
        const FString& InputName,
        FNiagaraVariable& OutVariable,
        TArray<FString>& OutDataInterfaceInputs)
    {
        TArray<FNiagaraVariable> Inputs;
        NiagaraEdit::EnumerateModuleStackInputs(ModuleNode, Inputs);

        bool bFound = false;
        for (const FNiagaraVariable& Variable : Inputs)
        {
            const FString ShortName = ShortStackInputName(Variable);
            if (Variable.GetType().IsDataInterface())
            {
                OutDataInterfaceInputs.Add(ShortName);
            }
            if (!bFound
                && (ShortName.Equals(InputName, ESearchCase::IgnoreCase)
                    || Variable.GetName().ToString().Equals(InputName, ESearchCase::IgnoreCase)))
            {
                OutVariable = Variable;
                bFound = true;
            }
        }
        return bFound;
    }

    // The module SCRIPT's own default object for this input: a Parameter-usage UNiagaraNodeInput
    // in the called graph carrying the DI. This is the object a stack shows as
    // `valueMode: "default"`, and it belongs to the module asset, not to the caller's emitter.
    UNiagaraDataInterface* FindScriptDefaultDataInterface(
        UNiagaraNodeFunctionCall& ModuleNode,
        const FNiagaraVariable& InputVariable)
    {
        UNiagaraScript* FunctionScript = ModuleNode.FunctionScript;
        UNiagaraGraph* CalledGraph = NiagaraJsonHelpers::GetGraphFromScript(FunctionScript);
        if (!CalledGraph)
        {
            return nullptr;
        }
        TArray<UNiagaraNodeInput*> InputNodes;
        CalledGraph->GetNodesOfClass<UNiagaraNodeInput>(InputNodes);
        for (UNiagaraNodeInput* InputNode : InputNodes)
        {
            if (InputNode
                && InputNode->Usage == ENiagaraInputNodeUsage::Parameter
                && InputNode->Input.GetName() == InputVariable.GetName())
            {
                if (UNiagaraDataInterface* DataInterface = GetInputNodeDataInterface(InputNode))
                {
                    return DataInterface;
                }
            }
        }
        return nullptr;
    }

    // The module's override pin for this input, if one exists. Matches the pin the way
    // ClassifyModuleInputBindings does, off the same canonical override node, so what this finds
    // and what the inspect / asset.dump `valueMode` readback reports cannot drift apart.
    UEdGraphPin* FindOverridePinForInput(
        UNiagaraNodeFunctionCall& ModuleNode,
        const FNiagaraParameterHandle& AliasedInputHandle)
    {
        UNiagaraNodeParameterMapSet* OverrideNode =
            NiagaraResetModuleInput::FindStackFunctionOverrideNode(ModuleNode);
        if (!OverrideNode)
        {
            return nullptr;
        }
        TArray<UEdGraphPin*> OverridePins;
        OverrideNode->GetInputPins(OverridePins);
        for (UEdGraphPin* Pin : OverridePins)
        {
            if (Pin && Pin->PinName == AliasedInputHandle.GetParameterHandleString())
            {
                return Pin;
            }
        }
        return nullptr;
    }
}

UNiagaraDataInterface* GetInputNodeDataInterface(const UNiagaraNodeInput* InputNode)
{
    if (!InputNode)
    {
        return nullptr;
    }
    static FObjectProperty* DataInterfaceProp = FindFProperty<FObjectProperty>(
        UNiagaraNodeInput::StaticClass(), TEXT("DataInterface"));
    if (!DataInterfaceProp)
    {
        return nullptr;
    }
    return Cast<UNiagaraDataInterface>(DataInterfaceProp->GetObjectPropertyValue_InContainer(InputNode));
}

FNiagaraEditError ResolveModuleInputDataInterface(
    FNiagaraResolvedTarget& Target,
    const FString& InputName,
    EResolveMode Mode,
    FResolvedModuleInputDI& Out)
{
    Out = FResolvedModuleInputDI();

    if (!Target.ModuleNode || !Target.Graph)
    {
        return FNiagaraEditError::Make(TEXT("MODULE_NOT_FOUND"), TEXT("Module target was not resolved."));
    }
    if (InputName.IsEmpty())
    {
        return FNiagaraEditError::Make(TEXT("INVALID_ARGUMENT"), TEXT("Missing 'inputName'."));
    }

    FNiagaraVariable InputVariable;
    TArray<FString> DataInterfaceInputs;
    if (!FindDeclaredStackInput(*Target.ModuleNode, InputName, InputVariable, DataInterfaceInputs))
    {
        // Name the DI-typed inputs rather than only refusing: the reason every spelling failed
        // before this fix was that no verb published the names that would work.
        const FString Available = DataInterfaceInputs.Num() > 0
            ? FString::Printf(TEXT(" Its data-interface inputs are: %s."), *FString::Join(DataInterfaceInputs, TEXT(", ")))
            : FString(TEXT(" It declares no data-interface inputs."));
        return FNiagaraEditError::Make(TEXT("MODULE_INPUT_NOT_FOUND"),
            FString::Printf(TEXT("Module '%s' declares no stack input '%s'.%s"),
                *Target.ModuleNode->GetFunctionName(), *InputName, *Available));
    }

    const FNiagaraTypeDefinition InputType = InputVariable.GetType();
    if (!InputType.IsDataInterface() || InputType.GetClass() == nullptr)
    {
        return FNiagaraEditError::Make(TEXT("INCOMPATIBLE_DATA_INTERFACE"),
            FString::Printf(TEXT("Module input '%s' is type '%s', which is not a data interface."),
                *InputName, *InputType.GetName()));
    }

    Out.DeclaredType = InputType;
    Out.ResolvedInputName = ShortStackInputName(InputVariable);

    const FNiagaraParameterHandle InputHandle =
        FNiagaraParameterHandle::CreateModuleParameterHandle(FName(*Out.ResolvedInputName));
    const FNiagaraParameterHandle AliasedInputHandle =
        FNiagaraParameterHandle::CreateAliasedModuleParameterHandle(InputHandle, Target.ModuleNode);

    UEdGraphPin* OverridePin = FindOverridePinForInput(*Target.ModuleNode, AliasedInputHandle);
    if (OverridePin && OverridePin->LinkedTo.Num() > 0 && OverridePin->LinkedTo[0])
    {
        UEdGraphNode* Upstream = OverridePin->LinkedTo[0]->GetOwningNode();
        if (const UNiagaraNodeInput* UpstreamInput = Cast<UNiagaraNodeInput>(Upstream))
        {
            if (UNiagaraDataInterface* OverrideDataInterface = GetInputNodeDataInterface(UpstreamInput))
            {
                Out.DataInterface = OverrideDataInterface;
                Out.ValueMode = TEXT("data");
                Out.bWritable = true;
                return FNiagaraEditError();
            }
        }

        // The override pin is driven by something that is not a data-interface value — a dynamic
        // input chain, a parameter link, an inline expression. There is no DI object to edit, and
        // replacing that driver is a different operation with its own disclosure rules, so refuse
        // and name what is there. Classified by the same walk the inspect readback uses.
        TMap<FName, NiagaraEdit::FModuleInputBindingInfo> Bindings;
        NiagaraEdit::ClassifyModuleInputBindings(*Target.ModuleNode, Bindings);
        const NiagaraEdit::FModuleInputBindingInfo* Binding = Bindings.Find(AliasedInputHandle.GetName());
        const FString BoundMode = Binding ? Binding->ValueMode : FString(TEXT("connected"));
        FString BoundSource;
        if (Binding)
        {
            BoundSource = !Binding->LinkedParameter.IsEmpty() ? Binding->LinkedParameter : Binding->DynamicInputScript;
        }
        return FNiagaraEditError::Make(TEXT("MODULE_INPUT_OVERRIDE_LINKED"),
            FString::Printf(
                TEXT("Module input '%s' is driven by an inbound link (valueMode '%s'%s%s), not by a data interface, ")
                TEXT("so there is no curve object to address. Clear it with niagara.reset_module_input first, ")
                TEXT("or edit the driving source instead."),
                *InputName,
                *BoundMode,
                BoundSource.IsEmpty() ? TEXT("") : TEXT(", source "),
                *BoundSource));
    }

    UNiagaraDataInterface* ScriptDefault = FindScriptDefaultDataInterface(*Target.ModuleNode, InputVariable);

    if (Mode == EResolveMode::Read)
    {
        if (!ScriptDefault)
        {
            return FNiagaraEditError::Make(TEXT("DATA_INTERFACE_NOT_FOUND"),
                FString::Printf(
                    TEXT("Module input '%s' carries no override value and its module script declares no default data interface."),
                    *InputName));
        }
        // Deliberately NOT writable: this object belongs to the module asset and is shared by
        // every placement of that module.
        Out.DataInterface = ScriptDefault;
        Out.ValueMode = TEXT("default");
        Out.bWritable = false;
        return FNiagaraEditError();
    }

    // Write path: give this placement its own DI. GetOrCreate returns the existing pin when one
    // is there but unlinked, which is the precondition SetDataInterfaceValueForFunctionInput
    // checkf()s on.
    UEdGraphPin& InputOverridePin = FNiagaraStackGraphUtilities::GetOrCreateStackFunctionInputOverridePin(
        *Target.ModuleNode,
        AliasedInputHandle,
        InputType,
        FGuid(),
        FGuid());
    InputOverridePin.Modify();

    UNiagaraDataInterface* CreatedDataInterface = nullptr;
    FNiagaraStackGraphUtilities::SetDataInterfaceValueForFunctionInput(
        InputOverridePin,
        InputType.GetClass(),
        AliasedInputHandle.GetParameterHandleString().ToString(),
        CreatedDataInterface);
    if (!CreatedDataInterface)
    {
        return FNiagaraEditError::Make(TEXT("CREATE_FAILED"),
            FString::Printf(TEXT("Failed to create an override data interface for module input '%s'."), *InputName));
    }

    // Seed from the module's authored default rather than leaving a class default behind: an edit
    // that rewrites one channel of a multi-channel curve must not silently blank the others, and a
    // caller reading the result back expects the shape the stack was showing.
    if (ScriptDefault && ScriptDefault->GetClass() == CreatedDataInterface->GetClass())
    {
        ScriptDefault->CopyTo(CreatedDataInterface);
    }

    Out.DataInterface = CreatedDataInterface;
    Out.ValueMode = TEXT("data");
    Out.bCreatedOverride = true;
    Out.bWritable = true;
    return FNiagaraEditError();
}
} // namespace NiagaraModuleInputDI
