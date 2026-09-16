// Copyright (c) 2026 Alexander Penkin. MIT License.

// Data-flow node emitters for NIR v1c (script-graph emission). One family file in the
// three-way dispatcher fan-out declared in NIRTextEmitter.h. Owns emission for:
//   UNiagaraNodeOp, UNiagaraNodeParameterMapGet, UNiagaraNodeParameterMapSet,
//   UNiagaraNodeFunctionCall, UNiagaraNodeInput, UNiagaraNodeOutput.
//
// Function-call nodes are emitted as references only ("call @ModuleName ..."); the
// called script's own nir.txt sidecar carries its inner graph. This matches the
// confirmed decision in the plan's Decisions section.

#include "NIR/NIRTextEmitter.h"

#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_Niagara.h"
#include "NiagaraCommon.h"
#include "NiagaraNode.h"
#include "NiagaraNodeCustomHlsl.h"
#include "NiagaraNodeFunctionCall.h"
#include "NiagaraNodeInput.h"
#include "NiagaraNodeOutput.h"
#include "NiagaraNodeOp.h"
#include "NiagaraNodeParameterMapGet.h"
#include "NiagaraNodeParameterMapSet.h"
#include "NiagaraNodeWithDynamicPins.h"
#include "NiagaraScript.h"
#include "NiagaraTypes.h"

namespace
{
    // SSA-style "%name" for the node's own output (used when other emitters need to
    // refer back to this node's value). Matches FormatPinValueRef's link form.
    FString FormatNodeOutputRef(const UEdGraphNode* Node, const UEdGraphPin* OutputPin)
    {
        if (!Node || !OutputPin)
        {
            return FString();
        }
        return FString::Printf(TEXT("%%%s.%s"), *Node->GetName(), *OutputPin->PinName.ToString());
    }

    bool IsParameterMapPin(const UEdGraphPin* Pin)
    {
        if (!Pin)
        {
            return false;
        }
        const FNiagaraTypeDefinition Type = UEdGraphSchema_Niagara::PinToTypeDefinition(Pin);
        return Type.IsValid() && Type == FNiagaraTypeDefinition::GetParameterMapDef();
    }

    // Vendored from UNiagaraNodeWithDynamicPins::IsAddPin (NiagaraNodeWithDynamicPins.cpp).
    // The class method is declared in the public header but is not DLL-exported (the class
    // has no NIAGARAEDITOR_API), so calling it across module boundaries fails to link on
    // UE 5.6. The static FName UNiagaraNodeWithDynamicPins::AddPinSubCategory is likewise
    // not exported; the engine initializes it to "DynamicAddPin" — we inline that literal
    // here to mirror the engine's IsAddPin body without depending on the unexported symbol.
    bool IsDynamicAddPin(const UEdGraphPin* Pin)
    {
        if (!Pin)
        {
            return false;
        }
        static const FName DynamicAddPinSubCategory(TEXT("DynamicAddPin"));
        return Pin->PinType.PinCategory == UEdGraphSchema_Niagara::PinCategoryMisc &&
            Pin->PinType.PinSubCategory == DynamicAddPinSubCategory;
    }

    void EmitOp(UNiagaraNodeOp* Node, FNIRTextEmitter& Out)
    {
        // Inputs are pin-typed; outputs feed downstream nodes. Op nodes have N inputs
        // and 1 output in practice but the dynamic-pin machinery allows more — emit
        // every output as a separate %result, and list every input.
        TArray<UEdGraphPin*> InputPins;
        TArray<UEdGraphPin*> OutputPins;
        Node->GetInputPins(InputPins);
        Node->GetOutputPins(OutputPins);

        TArray<FString> InputClauses;
        for (int32 Index = 0; Index < InputPins.Num(); ++Index)
        {
            UEdGraphPin* Pin = InputPins[Index];
            if (!Pin)
            {
                continue;
            }
            InputClauses.Add(FString::Printf(
                TEXT("input%d: %s"),
                Index,
                *FormatPinValueRef(Pin)));
        }

        // No output pin → fold to a single bare op line so the trace stays in the IR.
        if (OutputPins.IsEmpty())
        {
            Out.AppendLine(FString::Printf(
                TEXT("op %s(%s) %s"),
                *Node->OpName.ToString(),
                *FString::Join(InputClauses, TEXT(", ")),
                *NIRTextEmitter::FormatPositionSuffix(Node)));
            return;
        }

        for (UEdGraphPin* OutputPin : OutputPins)
        {
            if (!OutputPin)
            {
                continue;
            }
            Out.AppendLine(FString::Printf(
                TEXT("%s = op %s(%s) %s"),
                *FormatNodeOutputRef(Node, OutputPin),
                *Node->OpName.ToString(),
                *FString::Join(InputClauses, TEXT(", ")),
                *NIRTextEmitter::FormatPositionSuffix(Node)));
        }
    }

    void EmitParameterMapGet(UNiagaraNodeParameterMapGet* Node, FNIRTextEmitter& Out)
    {
        // One line per output pin: "%node.OutPin = get $Namespace.Name : Type @(x,y)".
        TArray<UEdGraphPin*> OutputPins;
        Node->GetOutputPins(OutputPins);
        for (UEdGraphPin* OutputPin : OutputPins)
        {
            // Skip the implicit map pin and the "Add" sentinel UI pin (DynamicAddPin sub-category):
            // the Add pin is a UI affordance for adding new typed parameters, has no FNiagaraTypeDefinition,
            // and the engine compile excludes it via IsValidPinToCompile. Mirroring that suppression here
            // avoids emitting "get $Add : Unknown" on every MapGet node.
            if (!OutputPin || IsParameterMapPin(OutputPin) || IsDynamicAddPin(OutputPin))
            {
                continue;
            }
            Out.AppendLine(FString::Printf(
                TEXT("%s = get %s : %s %s"),
                *FormatNodeOutputRef(Node, OutputPin),
                *NIRTextEmitter::FormatParameterRef(OutputPin->PinName),
                *NIRTextEmitter::FormatPinType(OutputPin),
                *NIRTextEmitter::FormatPositionSuffix(Node)));
        }
    }

    void EmitParameterMapSet(UNiagaraNodeParameterMapSet* Node, FNIRTextEmitter& Out)
    {
        // One line per input pin (skipping the implicit map pin): "set $X = %value @(x,y)".
        TArray<UEdGraphPin*> InputPins;
        Node->GetInputPins(InputPins);
        for (UEdGraphPin* InputPin : InputPins)
        {
            // Skip implicit map pin + Add sentinel; see EmitParameterMapGet for rationale.
            if (!InputPin || IsParameterMapPin(InputPin) || IsDynamicAddPin(InputPin))
            {
                continue;
            }
            Out.AppendLine(FString::Printf(
                TEXT("set %s = %s %s"),
                *NIRTextEmitter::FormatParameterRef(InputPin->PinName),
                *FormatPinValueRef(InputPin),
                *NIRTextEmitter::FormatPositionSuffix(Node)));
        }
    }

    void EmitFunctionCall(UNiagaraNodeFunctionCall* Node, FNIRTextEmitter& Out)
    {
        const FString ModuleName = Node->FunctionScript
            ? Node->FunctionScript->GetName()
            : Node->GetFunctionName();
        const FString VersionSuffix = NIRTextEmitter::FormatVersionSuffix(
            Node->SelectedScriptVersion, Node->FunctionScript);

        TArray<UEdGraphPin*> InputPins;
        Node->GetInputPins(InputPins);
        TArray<FString> InputClauses;
        for (UEdGraphPin* InputPin : InputPins)
        {
            if (!InputPin || IsParameterMapPin(InputPin))
            {
                continue;
            }
            const FString Expr = EmitInputValueExpr(InputPin, Out, 0);
            if (Expr.IsEmpty())
            {
                continue;
            }
            InputClauses.Add(FString::Printf(
                TEXT("input %s = %s"),
                *NIRTextEmitter::FormatNameToken(InputPin->PinName.ToString()),
                *Expr));
        }

        Out.AppendLine(FString::Printf(
            TEXT("call @%s%s (%s) %s"),
            *NIRTextEmitter::FormatNameToken(ModuleName),
            *VersionSuffix,
            *FString::Join(InputClauses, TEXT(", ")),
            *NIRTextEmitter::FormatPositionSuffix(Node)));
    }

    void EmitInput(UNiagaraNodeInput* Node, FNIRTextEmitter& Out)
    {
        // "%node.Out = input $Namespace.Name : Type @(x,y)". UNiagaraNodeInput exposes
        // its variable via .Input; we use that name for the parameter ref so the line
        // reads as the parameter being introduced, regardless of pin shape.
        TArray<UEdGraphPin*> OutputPins;
        Node->GetOutputPins(OutputPins);
        const FString ParamRef = NIRTextEmitter::FormatParameterRef(Node->Input.GetName());
        const FString TypeName = NIRTextEmitter::FormatTypeName(Node->Input.GetType());
        if (OutputPins.IsEmpty())
        {
            Out.AppendLine(FString::Printf(
                TEXT("input %s : %s %s"),
                *ParamRef,
                *TypeName,
                *NIRTextEmitter::FormatPositionSuffix(Node)));
            return;
        }
        for (UEdGraphPin* OutputPin : OutputPins)
        {
            if (!OutputPin)
            {
                continue;
            }
            Out.AppendLine(FString::Printf(
                TEXT("%s = input %s : %s %s"),
                *FormatNodeOutputRef(Node, OutputPin),
                *ParamRef,
                *TypeName,
                *NIRTextEmitter::FormatPositionSuffix(Node)));
        }
    }

    void EmitOutput(UNiagaraNodeOutput* Node, FNIRTextEmitter& Out)
    {
        // "output <UsageName> (slot0: %A, slot1: %B) @(x,y)". The slots are the input
        // pins on the output node (one per declared variable in the script's Outputs).
        TArray<UEdGraphPin*> InputPins;
        Node->GetInputPins(InputPins);
        TArray<FString> SlotClauses;
        int32 SlotIndex = 0;
        for (UEdGraphPin* InputPin : InputPins)
        {
            if (!InputPin || IsParameterMapPin(InputPin))
            {
                continue;
            }
            SlotClauses.Add(FString::Printf(
                TEXT("slot%d: %s"),
                SlotIndex,
                *FormatPinValueRef(InputPin)));
            ++SlotIndex;
        }

        // Route through the shared ENiagaraScriptUsage→token formatter so the mapping
        // table lives in one place. When the helper returns "Unknown" the enum value
        // wasn't in its switch — surface that as a warning so coverage gaps are visible
        // in FNIRResult.Warnings instead of emitting a clean-looking line silently.
        const FString UsageName = NIRTextEmitter::FormatUsageName(Node->GetUsage());
        if (UsageName == TEXT("Unknown"))
        {
            Out.Warn(FString::Printf(
                TEXT("EmitOutput: unmapped ENiagaraScriptUsage value %d"),
                int32(Node->GetUsage())));
        }

        Out.AppendLine(FString::Printf(
            TEXT("output %s (%s) %s"),
            *UsageName,
            *FString::Join(SlotClauses, TEXT(", ")),
            *NIRTextEmitter::FormatPositionSuffix(Node)));
    }
}

bool NIRGraphEmit_Dataflow(UNiagaraNode* Node, FNIRTextEmitter& Out)
{
    if (!Node)
    {
        return false;
    }
    if (UNiagaraNodeOp* OpNode = Cast<UNiagaraNodeOp>(Node))
    {
        EmitOp(OpNode, Out);
        return true;
    }
    // UNiagaraNodeParameterMapGet and UNiagaraNodeParameterMapSet have no NIAGARAEDITOR_API;
    // resolve their UClass via reflection to avoid unresolved GetPrivateStaticClass link errors.
    // Order matters: ParameterMapSet is also a ParameterMapBase; cast to Set/Get directly
    // so the more specific class wins. Neither inherits from the other.
    UClass* ParameterMapSetClass = FindObject<UClass>(nullptr, TEXT("/Script/NiagaraEditor.NiagaraNodeParameterMapSet"));
    UClass* ParameterMapGetClass = FindObject<UClass>(nullptr, TEXT("/Script/NiagaraEditor.NiagaraNodeParameterMapGet"));
    if (ParameterMapSetClass && Node->IsA(ParameterMapSetClass))
    {
        EmitParameterMapSet(static_cast<UNiagaraNodeParameterMapSet*>(Node), Out);
        return true;
    }
    if (ParameterMapGetClass && Node->IsA(ParameterMapGetClass))
    {
        EmitParameterMapGet(static_cast<UNiagaraNodeParameterMapGet*>(Node), Out);
        return true;
    }
    // UNiagaraNodeCustomHlsl inherits from UNiagaraNodeFunctionCall; defer it to the
    // Util family so the customHlsl block emission wins. The Dataflow function-call
    // handler is only for plain (non-Hlsl) script calls.
    if (UNiagaraNodeFunctionCall* CallNode = Cast<UNiagaraNodeFunctionCall>(Node))
    {
        if (Node->IsA<UNiagaraNodeCustomHlsl>())
        {
            return false;
        }
        EmitFunctionCall(CallNode, Out);
        return true;
    }
    if (UNiagaraNodeInput* InputNode = Cast<UNiagaraNodeInput>(Node))
    {
        EmitInput(InputNode, Out);
        return true;
    }
    if (UNiagaraNodeOutput* OutputNode = Cast<UNiagaraNodeOutput>(Node))
    {
        EmitOutput(OutputNode, Out);
        return true;
    }
    return false;
}
