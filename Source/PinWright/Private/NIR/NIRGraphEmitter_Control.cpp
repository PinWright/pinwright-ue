// Copyright (c) 2026 Alexander Penkin. MIT License.

// Control-flow node emitters for NIR v1c (F-niagara-decompile-nir-script-graphs).
//
// Owns dispatch + emission for the "control" cluster of Niagara graph nodes:
//   - UNiagaraNodeStaticSwitch        — compile-time branch selector ("staticSwitch ...").
//   - UNiagaraNodeIf                  — runtime two-way branch ("if (%cond) then ... else ... @(x, y)").
//   - UNiagaraNodeSelect              — N-way runtime selector ("select ...").
//   - UNiagaraNodeUsageSelector       — per-usage branch ("selectUsage ..."), symbolic case labels.
//   - UNiagaraNodeSimTargetSelector   — CPU vs GPU sim-target branch ("selectSimTarget ..."), symbolic case labels.
//
// Returns true when Node is handled here so EmitGraphBody short-circuits before
// trying the next family / unknown-node fallback. Pin wiring uses FormatPinValueRef
// (declared in NIRTextEmitter.h) so the SSA-style "%upstream.OutputPinName" form is
// shared with the dataflow / util families. UNiagaraNodeSimTargetSelector derives
// from UNiagaraNodeUsageSelector and UNiagaraNodeSelect / UNiagaraNodeStaticSwitch
// also derive from it; the cast order below intentionally tests the most-derived
// subclass first so the more-specific emission shape is chosen.

#include "NIR/NIRTextEmitter.h"

#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "NiagaraCommon.h"
#include "NiagaraNode.h"
#include "NiagaraNodeIf.h"
#include "NiagaraNodeSelect.h"
#include "NiagaraNodeSimTargetSelector.h"
#include "NiagaraNodeStaticSwitch.h"
#include "NiagaraNodeUsageSelector.h"
#include "NiagaraTypes.h"
#include "UObject/Class.h"

namespace
{
    // Format a static-switch selector type token. The switch carries either bool,
    // integer, or enum semantics in SwitchTypeData; "enum:<EnumName>" keeps the
    // emitted line self-describing without inlining the full enum schema.
    FString FormatStaticSwitchType(const UNiagaraNodeStaticSwitch& Node)
    {
        switch (Node.SwitchTypeData.SwitchType)
        {
        case ENiagaraStaticSwitchType::Bool:
            return TEXT("bool");
        case ENiagaraStaticSwitchType::Integer:
            return TEXT("int");
        case ENiagaraStaticSwitchType::Enum:
            return FString::Printf(
                TEXT("enum:%s"),
                Node.SwitchTypeData.Enum ? *Node.SwitchTypeData.Enum->GetName() : TEXT("?"));
        }
        return TEXT("?");
    }

    // UNiagaraNodeStaticSwitch enumerates its input pins in option-major order:
    // for each option value (in GetOptionValues() order) one pin per output var.
    // We emit one "case <Value>: %out = %src" line per (option, output) pair.
    //
    // Note: NiagaraDumpBuilder::BuildStaticSwitchInputs is NOT applicable here —
    // that helper takes a UNiagaraNodeFunctionCall* and walks static-switch nodes
    // inside the called subgraph to decode the caller's override pins. The case
    // we handle here is a UNiagaraNodeStaticSwitch sitting directly in the graph
    // being walked; the pin layout is the source of truth.
    void EmitStaticSwitch(UNiagaraNodeStaticSwitch& Node, FNIRTextEmitter& Out)
    {
        const FString SwitchName = Node.InputParameterName.IsNone()
            ? FString(TEXT("?"))
            : Node.InputParameterName.ToString();
        const FString Header = FString::Printf(
            TEXT("staticSwitch $%s : %s %s "),
            *SwitchName,
            *FormatStaticSwitchType(Node),
            *NIRTextEmitter::FormatPositionSuffix(&Node));
        Out.EnterScope(Header);

        const TArray<int32> OptionValues = Node.GetOptionValues();
        const TArray<FNiagaraVariable>& OutputVars = Node.OutputVars;

        // Pin layout: option-major. For option index O and output index V the
        // input pin index is (O * OutputVars.Num() + V). Walk Node.Pins by
        // direction-filtered input list to recover the same order.
        TArray<UEdGraphPin*> InputPins;
        for (UEdGraphPin* Pin : Node.Pins)
        {
            if (Pin && Pin->Direction == EGPD_Input)
            {
                InputPins.Add(Pin);
            }
        }

        for (int32 OptionIndex = 0; OptionIndex < OptionValues.Num(); ++OptionIndex)
        {
            for (int32 OutputIndex = 0; OutputIndex < OutputVars.Num(); ++OutputIndex)
            {
                const int32 PinIndex = OptionIndex * OutputVars.Num() + OutputIndex;
                if (!InputPins.IsValidIndex(PinIndex))
                {
                    continue;
                }
                UEdGraphPin* InputPin = InputPins[PinIndex];
                const FString OutputName = OutputVars[OutputIndex].GetName().ToString();
                Out.AppendLine(FString::Printf(
                    TEXT("case %d: %%%s = %s"),
                    OptionValues[OptionIndex],
                    *OutputName,
                    *FormatPinValueRef(InputPin)));
            }
        }
        Out.ExitScope();
    }

    // UNiagaraNodeIf carries OutputVars + PathAssociatedPinGuids (one tuple per
    // output variable). The spec form is a single line:
    //   if (%cond) then %thenA, %thenB else %elseA, %elseB @(x, y)
    // where the then-list / else-list are the RHS expressions for each output
    // variable in declaration order. Output names are dropped from the comma-list
    // form — the spec compresses them to keep the line readable.
    void EmitIf(UNiagaraNodeIf& Node, FNIRTextEmitter& Out)
    {
        // Pre-build the GUID → pin map once. The per-path GUID lookups otherwise
        // do N×M scans across Node.Pins.
        TMap<FGuid, UEdGraphPin*> PinByGuid;
        PinByGuid.Reserve(Node.Pins.Num());
        for (UEdGraphPin* Pin : Node.Pins)
        {
            if (Pin)
            {
                PinByGuid.Add(Pin->PinId, Pin);
            }
        }

        UEdGraphPin* const* ConditionPinPtr = PinByGuid.Find(Node.ConditionPinGuid);
        UEdGraphPin* ConditionPin = ConditionPinPtr ? *ConditionPinPtr : nullptr;
        const FString CondExpr = ConditionPin ? FormatPinValueRef(ConditionPin) : FString();

        TArray<FString> ThenExprs;
        TArray<FString> ElseExprs;
        ThenExprs.Reserve(Node.OutputVars.Num());
        ElseExprs.Reserve(Node.OutputVars.Num());
        for (int32 Index = 0; Index < Node.OutputVars.Num(); ++Index)
        {
            if (!Node.PathAssociatedPinGuids.IsValidIndex(Index))
            {
                continue;
            }
            const FPinGuidsForPath& Path = Node.PathAssociatedPinGuids[Index];
            UEdGraphPin* const* ThenPinPtr = PinByGuid.Find(Path.InputTruePinGuid);
            UEdGraphPin* const* ElsePinPtr = PinByGuid.Find(Path.InputFalsePinGuid);
            ThenExprs.Add(ThenPinPtr && *ThenPinPtr ? FormatPinValueRef(*ThenPinPtr) : FString());
            ElseExprs.Add(ElsePinPtr && *ElsePinPtr ? FormatPinValueRef(*ElsePinPtr) : FString());
        }

        Out.AppendLine(FString::Printf(
            TEXT("if (%s) then %s else %s %s"),
            *CondExpr,
            *FString::Join(ThenExprs, TEXT(", ")),
            *FString::Join(ElseExprs, TEXT(", ")),
            *NIRTextEmitter::FormatPositionSuffix(&Node)));
    }

    // UNiagaraNodeSelect: per-option input pins fan into one output per OutputVar.
    // The selector pin's name carries the chooser parameter. Emission lays out
    // each (option, output) pair the same way as StaticSwitch so the consumer
    // can read both block shapes the same way.
    void EmitSelect(UNiagaraNodeSelect& Node, FNIRTextEmitter& Out)
    {
        UEdGraphPin* SelectorPin = nullptr;
        TArray<UEdGraphPin*> InputPins;
        for (UEdGraphPin* Pin : Node.Pins)
        {
            if (!Pin || Pin->Direction != EGPD_Input)
            {
                continue;
            }
            if (Pin->PersistentGuid == Node.SelectorPinGuid)
            {
                SelectorPin = Pin;
                continue;
            }
            if (!Pin->bOrphanedPin)
            {
                InputPins.Add(Pin);
            }
        }

        const FString SelectorName = SelectorPin
            ? SelectorPin->PinName.ToString()
            : FString(TEXT("?"));
        const FString Header = FString::Printf(
            TEXT("select $%s %s "),
            *SelectorName,
            *NIRTextEmitter::FormatPositionSuffix(&Node));
        Out.EnterScope(Header);

        const TArray<int32> OptionValues = Node.GetOptionValues();
        const TArray<FNiagaraVariable>& OutputVars = Node.OutputVars;
        for (int32 OptionIndex = 0; OptionIndex < OptionValues.Num(); ++OptionIndex)
        {
            for (int32 OutputIndex = 0; OutputIndex < OutputVars.Num(); ++OutputIndex)
            {
                const int32 PinIndex = OptionIndex * OutputVars.Num() + OutputIndex;
                if (!InputPins.IsValidIndex(PinIndex))
                {
                    continue;
                }
                const FString OutputName = OutputVars[OutputIndex].GetName().ToString();
                Out.AppendLine(FString::Printf(
                    TEXT("case %d: %%%s = %s"),
                    OptionValues[OptionIndex],
                    *OutputName,
                    *FormatPinValueRef(InputPins[PinIndex])));
            }
        }
        Out.ExitScope();
    }

    // Shared body for UNiagaraNodeUsageSelector / UNiagaraNodeSimTargetSelector.
    // Both carry OutputVars + option-major input pin layout via GetOptionValues();
    // the only difference is how option integers map to symbolic case labels
    // (UsageSelector → ENiagaraScriptUsage, SimTargetSelector → ENiagaraSimTarget).
    // Callers pass a mapper TFunction; an empty return falls back to "case <int>:"
    // plus a warning so coverage holes surface in tests.
    void EmitOptionMajorSelector(
        UNiagaraNodeUsageSelector& Node,
        FNIRTextEmitter& Out,
        const TCHAR* HeaderKeyword,
        TFunctionRef<FString(int32)> CaseLabelMapper)
    {
        const FString Header = FString::Printf(
            TEXT("%s %s "),
            HeaderKeyword,
            *NIRTextEmitter::FormatPositionSuffix(&Node));
        Out.EnterScope(Header);

        const TArray<int32> OptionValues = Node.GetOptionValues();
        const TArray<FNiagaraVariable>& OutputVars = Node.OutputVars;

        TArray<UEdGraphPin*> InputPins;
        for (UEdGraphPin* Pin : Node.Pins)
        {
            if (Pin && Pin->Direction == EGPD_Input)
            {
                InputPins.Add(Pin);
            }
        }

        for (int32 OptionIndex = 0; OptionIndex < OptionValues.Num(); ++OptionIndex)
        {
            const int32 OptionValue = OptionValues[OptionIndex];
            FString Label = CaseLabelMapper(OptionValue);
            const bool bUnmapped = Label.IsEmpty();
            if (bUnmapped)
            {
                Label = FString::Printf(TEXT("%d"), OptionValue);
                Out.Warn(FString::Printf(
                    TEXT("EmitOptionMajorSelector: %s option value %d has no symbolic label; emitted raw integer."),
                    HeaderKeyword,
                    OptionValue));
            }
            for (int32 OutputIndex = 0; OutputIndex < OutputVars.Num(); ++OutputIndex)
            {
                const int32 PinIndex = OptionIndex * OutputVars.Num() + OutputIndex;
                if (!InputPins.IsValidIndex(PinIndex))
                {
                    continue;
                }
                const FString OutputName = OutputVars[OutputIndex].GetName().ToString();
                Out.AppendLine(FString::Printf(
                    TEXT("case %s: %%%s = %s"),
                    *Label,
                    *OutputName,
                    *FormatPinValueRef(InputPins[PinIndex])));
            }
        }
        Out.ExitScope();
    }

    void EmitUsageSelector(UNiagaraNodeUsageSelector& Node, FNIRTextEmitter& Out)
    {
        UEnum* UsageEnum = StaticEnum<ENiagaraScriptUsage>();
        EmitOptionMajorSelector(
            Node, Out, TEXT("selectUsage"),
            [UsageEnum](int32 OptionValue) -> FString
            {
                return UsageEnum ? UsageEnum->GetNameStringByValue(OptionValue) : FString();
            });
    }

    void EmitSimTargetSelector(UNiagaraNodeSimTargetSelector& Node, FNIRTextEmitter& Out)
    {
        UEnum* SimTargetEnum = StaticEnum<ENiagaraSimTarget>();
        EmitOptionMajorSelector(
            Node, Out, TEXT("selectSimTarget"),
            [SimTargetEnum](int32 OptionValue) -> FString
            {
                return SimTargetEnum ? SimTargetEnum->GetNameStringByValue(OptionValue) : FString();
            });
    }
}

bool NIRGraphEmit_Control(UNiagaraNode* Node, FNIRTextEmitter& Out)
{
    if (!Node)
    {
        return false;
    }
    // Test most-derived classes first. UNiagaraNodeStaticSwitch and
    // UNiagaraNodeSelect both derive from UNiagaraNodeUsageSelector, and
    // UNiagaraNodeSimTargetSelector also derives from it; running the
    // base-class cast last preserves the per-subclass emission shape.
    if (UNiagaraNodeStaticSwitch* StaticSwitch = Cast<UNiagaraNodeStaticSwitch>(Node))
    {
        EmitStaticSwitch(*StaticSwitch, Out);
        return true;
    }
    if (UNiagaraNodeIf* If = Cast<UNiagaraNodeIf>(Node))
    {
        EmitIf(*If, Out);
        return true;
    }
    if (UNiagaraNodeSelect* Select = Cast<UNiagaraNodeSelect>(Node))
    {
        EmitSelect(*Select, Out);
        return true;
    }
    if (UNiagaraNodeSimTargetSelector* SimTarget = Cast<UNiagaraNodeSimTargetSelector>(Node))
    {
        EmitSimTargetSelector(*SimTarget, Out);
        return true;
    }
    if (UNiagaraNodeUsageSelector* Usage = Cast<UNiagaraNodeUsageSelector>(Node))
    {
        EmitUsageSelector(*Usage, Out);
        return true;
    }
    return false;
}
