// Copyright (c) 2026 Alexander Penkin. MIT License.

// Utility-family node emitters for NIR v1c (F-niagara-decompile-nir-script-graphs).
//
// Owns dispatch + emission for the "utility" cluster of Niagara graph nodes:
//   - UNiagaraNodeCustomHlsl  — verbatim HLSL block, declared inputs / outputs above body.
//   - UNiagaraNodeConvert     — pin wiring (surface input pins only; sub-path / sub-field
//                               convert mappings are out of scope per
//                               feedback_ir_logical_not_visual — logical equivalence only).
//   - UNiagaraNodeReroute     — pass-through marker. Per feedback_ir_logical_not_visual,
//                               reroute is logically a no-op; we still emit it for traceability.
//
// Returns true when Node is handled here so EmitGraphBody short-circuits before
// trying the next family / unknown-node fallback. Pin wiring uses FormatPinValueRef
// (declared in NIRTextEmitter.h) so the SSA-style "%upstream.OutputPinName" form is
// shared with the dataflow / control families.

#include "NIR/NIRTextEmitter.h"

#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "NiagaraNode.h"
#include "NiagaraNodeConvert.h"
#include "NiagaraNodeCustomHlsl.h"
#include "NiagaraNodeReroute.h"
#include "UObject/UnrealType.h"

namespace
{
    FString GetCustomHlslText(const UNiagaraNodeCustomHlsl& Node)
    {
        if (FStrProperty* CustomHlslProperty = FindFProperty<FStrProperty>(
                Node.GetClass(),
                FName(TEXT("CustomHlsl"))))
        {
            return CustomHlslProperty->GetPropertyValue_InContainer(&Node);
        }
        return FString();
    }

    // CustomHlsl emits a multi-line "customHlsl @(x, y) { ... }" block. Inputs and
    // outputs are declared above the verbatim HLSL body so callers can correlate
    // the HLSL source to the surrounding NIR dataflow without parsing HLSL.
    void EmitCustomHlsl(UNiagaraNodeCustomHlsl& Node, FNIRTextEmitter& Out)
    {
        const FString Header = FString::Printf(
            TEXT("customHlsl %s "),
            *NIRTextEmitter::FormatPositionSuffix(&Node));
        Out.EnterScope(Header);

        for (UEdGraphPin* Pin : Node.Pins)
        {
            if (!Pin)
            {
                continue;
            }
            if (Pin->Direction == EGPD_Input)
            {
                // Unlinked input pins on a CustomHlsl block carry no RHS literal we
                // can meaningfully emit (the HLSL body references them by name and
                // pulls the value at compile time, not the pin default). Emit the
                // declaration only when the pin is unlinked.
                if (Pin->LinkedTo.IsEmpty())
                {
                    Out.AppendLine(FString::Printf(
                        TEXT("input %s : %s"),
                        *NIRTextEmitter::FormatNameToken(Pin->PinName),
                        *NIRTextEmitter::FormatPinType(Pin)));
                }
                else
                {
                    Out.AppendLine(FString::Printf(
                        TEXT("input %s : %s = %s"),
                        *NIRTextEmitter::FormatNameToken(Pin->PinName),
                        *NIRTextEmitter::FormatPinType(Pin),
                        *FormatPinValueRef(Pin)));
                }
            }
            else if (Pin->Direction == EGPD_Output)
            {
                Out.AppendLine(FString::Printf(
                    TEXT("output %s : %s"),
                    *NIRTextEmitter::FormatNameToken(Pin->PinName),
                    *NIRTextEmitter::FormatPinType(Pin)));
            }
        }

        // The verbatim HLSL source may span multiple lines. Emit each line at the
        // current indent so the block stays human-readable.
        const FString Hlsl = GetCustomHlslText(Node);
        TArray<FString> HlslLines;
        Hlsl.ParseIntoArrayLines(HlslLines, /*bCullEmpty=*/false);
        for (const FString& Line : HlslLines)
        {
            Out.AppendLine(Line);
        }

        Out.ExitScope();
    }

    // Convert nodes carry an internal wiring map of input pin sub-paths to output
    // pin sub-paths. Per feedback_ir_logical_not_visual, NIR v1c emits only the
    // surface-level input-pin RHS values; the per-sub-path remap is internal state
    // we deliberately skip (the full convert map would land if a v2 round-trip ever
    // needs it).
    void EmitConvert(UNiagaraNodeConvert& Node, FNIRTextEmitter& Out)
    {
        TArray<FString> InputRefs;
        for (UEdGraphPin* InputPin : Node.Pins)
        {
            if (!InputPin || InputPin->Direction != EGPD_Input)
            {
                continue;
            }
            InputRefs.Add(FormatPinValueRef(InputPin));
        }
        Out.AppendLine(FString::Printf(
            TEXT("convert (%s) %s"),
            *FString::Join(InputRefs, TEXT(", ")),
            *NIRTextEmitter::FormatPositionSuffix(&Node)));
    }

    // Reroute is a logical pass-through. Emitted for traceability so the surrounding
    // dataflow stays readable, but a downstream consumer is free to elide the line —
    // see feedback_ir_logical_not_visual.
    void EmitReroute(UNiagaraNodeReroute& Node, FNIRTextEmitter& Out)
    {
        UEdGraphPin* InputPin = nullptr;
        UEdGraphPin* OutputPin = nullptr;
        for (UEdGraphPin* Pin : Node.Pins)
        {
            if (!Pin) continue;
            if (Pin->Direction == EGPD_Input && !InputPin) InputPin = Pin;
            else if (Pin->Direction == EGPD_Output && !OutputPin) OutputPin = Pin;
        }
        const FString InputRef = InputPin ? FormatPinValueRef(InputPin) : FString();
        // Use node-qualified SSA form ("%NodeName.OutputPinName") matching
        // FormatNodeOutputRef in NIRGraphEmitter_Dataflow.cpp so downstream
        // references resolve uniquely across the graph.
        const FString OutputRef = OutputPin
            ? FString::Printf(TEXT("%%%s.%s"), *Node.GetName(), *OutputPin->PinName.ToString())
            : FString(TEXT("%out"));
        Out.AppendLine(FString::Printf(
            TEXT("reroute %s -> %s %s"),
            *InputRef,
            *OutputRef,
            *NIRTextEmitter::FormatPositionSuffix(&Node)));
    }
}

bool NIRGraphEmit_Util(UNiagaraNode* Node, FNIRTextEmitter& Out)
{
    if (!Node)
    {
        return false;
    }
    if (UNiagaraNodeCustomHlsl* CustomHlsl = Cast<UNiagaraNodeCustomHlsl>(Node))
    {
        EmitCustomHlsl(*CustomHlsl, Out);
        return true;
    }
    // UNiagaraNodeConvert has no NIAGARAEDITOR_API; resolve its UClass via reflection
    // to avoid an unresolved GetPrivateStaticClass link error on UE 5.4-5.7.
    UClass* NiagaraNodeConvertClass = FindObject<UClass>(nullptr, TEXT("/Script/NiagaraEditor.NiagaraNodeConvert"));
    if (NiagaraNodeConvertClass && Node->IsA(NiagaraNodeConvertClass))
    {
        EmitConvert(*static_cast<UNiagaraNodeConvert*>(Node), Out);
        return true;
    }
    if (UNiagaraNodeReroute* Reroute = Cast<UNiagaraNodeReroute>(Node))
    {
        EmitReroute(*Reroute, Out);
        return true;
    }
    return false;
}
