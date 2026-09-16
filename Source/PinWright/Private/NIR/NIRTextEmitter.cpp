// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "NIR/NIRTextEmitter.h"

#include "IrCore/IrTextUtils.h"

#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_Niagara.h"
#include "NiagaraCommon.h"
#include "NiagaraGraph.h"
#include "NiagaraNode.h"
#include "NiagaraNodeFunctionCall.h"
#include "NiagaraNodeInput.h"
#include "NiagaraScript.h"
#include "NiagaraTypes.h"
#include "Templates/SharedPointer.h"
#include "ViewModels/Stack/NiagaraParameterHandle.h"

#include "Handlers/Niagara/NiagaraJsonHelpers.h"

namespace
{
    constexpr int32 GNIRIndentSpaces = 4;

    // Module input chains can be authored arbitrarily deep through dynamic-input
    // function-calls. The Niagara editor enforces DAG override chains in practice,
    // but a malformed asset (or a corrupted graph) could form a cycle. Depth-32 is
    // far beyond what any real authoring would produce and keeps emission bounded.
    constexpr int32 GNIRMaxOverrideDepth = 32;

    FString FormatTypeObjectName(const UObject* TypeObject)
    {
        if (const UStruct* Struct = Cast<UStruct>(TypeObject))
        {
            return Struct->GetName();
        }
        if (const UEnum* Enum = Cast<UEnum>(TypeObject))
        {
            return Enum->GetName();
        }
        return FString();
    }

    FString FormatGraphNodeDisplayName(const UEdGraphNode* Node)
    {
        if (!Node)
        {
            return FString(TEXT("Node"));
        }
        FString DisplayName = Node->GetNodeTitle(ENodeTitleType::ListView).ToString();
        if (DisplayName.IsEmpty())
        {
            DisplayName = Node->GetName();
        }
        return NIRTextEmitter::FormatNameToken(DisplayName);
    }

    TMap<const UEdGraphNode*, FString> BuildGraphNodeNames(const UNiagaraGraph& Graph)
    {
        TMap<FString, int32> NameCounts;
        TMap<const UEdGraphNode*, FString> Result;
        for (UEdGraphNode* EdNode : Graph.Nodes)
        {
            UNiagaraNode* Node = Cast<UNiagaraNode>(EdNode);
            if (!Node)
            {
                continue;
            }
            const FString BaseName = FormatGraphNodeDisplayName(Node);
            int32& Count = NameCounts.FindOrAdd(BaseName);
            ++Count;
            Result.Add(Node, Count == 1 ? BaseName : FString::Printf(TEXT("%s_%d"), *BaseName, Count));
        }
        return Result;
    }

    void EmitGraphNodeDeclarations(UNiagaraGraph& Graph, const TMap<const UEdGraphNode*, FString>& NodeNames, FNIRTextEmitter& Out)
    {
        for (UEdGraphNode* EdNode : Graph.Nodes)
        {
            UNiagaraNode* Node = Cast<UNiagaraNode>(EdNode);
            if (!Node)
            {
                continue;
            }
            const FString* NodeName = NodeNames.Find(Node);
            if (!NodeName)
            {
                continue;
            }
            Out.AppendLine(FString::Printf(
                TEXT("node %s : %s %s"),
                **NodeName,
                *NIRTextEmitter::FormatNameToken(Node->GetClass()->GetName()),
                *NIRTextEmitter::FormatPositionSuffix(Node)));
        }
    }

    void EmitGraphLinks(UNiagaraGraph& Graph, const TMap<const UEdGraphNode*, FString>& NodeNames, FNIRTextEmitter& Out)
    {
        TArray<FString> LinkLines;
        for (UEdGraphNode* EdNode : Graph.Nodes)
        {
            UNiagaraNode* FromNode = Cast<UNiagaraNode>(EdNode);
            if (!FromNode)
            {
                continue;
            }
            const FString* FromNodeName = NodeNames.Find(FromNode);
            if (!FromNodeName)
            {
                continue;
            }
            for (UEdGraphPin* FromPin : FromNode->Pins)
            {
                if (!FromPin || FromPin->Direction != EGPD_Output)
                {
                    continue;
                }
                for (UEdGraphPin* ToPin : FromPin->LinkedTo)
                {
                    UEdGraphNode* ToEdNode = ToPin ? ToPin->GetOwningNode() : nullptr;
                    const FString* ToNodeName = ToEdNode ? NodeNames.Find(ToEdNode) : nullptr;
                    if (!ToPin || !ToNodeName)
                    {
                        continue;
                    }
                    LinkLines.Add(FString::Printf(
                        TEXT("link %s.%s -> %s.%s"),
                        **FromNodeName,
                        *NIRTextEmitter::FormatNameToken(FromPin->PinName.ToString()),
                        **ToNodeName,
                        *NIRTextEmitter::FormatNameToken(ToPin->PinName.ToString())));
                }
            }
        }
        LinkLines.Sort();
        for (const FString& Line : LinkLines)
        {
            Out.AppendLine(Line);
        }
    }
}

void FNIRTextEmitter::AppendLine(FStringView Line)
{
    AppendIndent();
    Builder.Append(Line);
    Builder.Append(TEXT("\n"));
}

void FNIRTextEmitter::AppendIndent()
{
    for (int32 Index = 0; Index < IndentDepth * GNIRIndentSpaces; ++Index)
    {
        Builder.AppendChar(TEXT(' '));
    }
}

void FNIRTextEmitter::EnterScope(FStringView Header)
{
    AppendIndent();
    Builder.Append(Header);
    Builder.Append(TEXT("{\n"));
    ++IndentDepth;
}

void FNIRTextEmitter::ExitScope()
{
    IndentDepth = FMath::Max(0, IndentDepth - 1);
    AppendIndent();
    Builder.Append(TEXT("}\n"));
}

void FNIRTextEmitter::Warn(FStringView Message)
{
    if (OutWarnings)
    {
        OutWarnings->Add(FString(Message));
    }
}

FString FNIRTextEmitter::ToString()
{
    return FString(Builder.ToString());
}

namespace NIRTextEmitter
{
    FString Quote(FStringView Value)
    {
        return FIrTextUtils::Quote(FString(Value));
    }

    FString FormatNameToken(FName Name)
    {
        return FIrTextUtils::FormatNameToken(Name.ToString());
    }

    FString FormatNameToken(FStringView Name)
    {
        return FIrTextUtils::FormatNameToken(FString(Name));
    }

    FString FormatPositionSuffix(int32 X, int32 Y)
    {
        return FIrTextUtils::FormatPositionSuffix(X, Y);
    }

    FString FormatPositionSuffix(const UEdGraphNode* Node)
    {
        if (!Node)
        {
            return FString();
        }
        return FormatPositionSuffix(Node->NodePosX, Node->NodePosY);
    }

    FString FormatParameterRef(FName ParameterHandleName)
    {
        const FNiagaraParameterHandle Handle(ParameterHandleName);
        const FName Namespace = Handle.GetNamespace();
        const FName Name = Handle.GetName();
        if (!Namespace.IsNone() && !Name.IsNone())
        {
            return FString::Printf(TEXT("$%s.%s"), *Namespace.ToString(), *Name.ToString());
        }
        return FString::Printf(TEXT("$%s"), *ParameterHandleName.ToString());
    }

    FString FormatUsageName(ENiagaraScriptUsage Usage)
    {
        switch (Usage)
        {
        case ENiagaraScriptUsage::Function: return TEXT("Function");
        case ENiagaraScriptUsage::Module: return TEXT("Module");
        case ENiagaraScriptUsage::DynamicInput: return TEXT("DynamicInput");
        case ENiagaraScriptUsage::ParticleSpawnScript: return TEXT("ParticleSpawn");
        case ENiagaraScriptUsage::ParticleSpawnScriptInterpolated: return TEXT("ParticleSpawnInterpolated");
        case ENiagaraScriptUsage::ParticleUpdateScript: return TEXT("ParticleUpdate");
        case ENiagaraScriptUsage::ParticleEventScript: return TEXT("ParticleEvent");
        case ENiagaraScriptUsage::ParticleSimulationStageScript: return TEXT("ParticleSimulationStage");
        case ENiagaraScriptUsage::ParticleGPUComputeScript: return TEXT("ParticleGPUCompute");
        case ENiagaraScriptUsage::EmitterSpawnScript: return TEXT("EmitterSpawn");
        case ENiagaraScriptUsage::EmitterUpdateScript: return TEXT("EmitterUpdate");
        case ENiagaraScriptUsage::SystemSpawnScript: return TEXT("SystemSpawn");
        case ENiagaraScriptUsage::SystemUpdateScript: return TEXT("SystemUpdate");
        default: return TEXT("Unknown");
        }
    }

    FString FormatTypeName(const FNiagaraTypeDefinition& Type)
    {
        if (!Type.IsValid())
        {
            return TEXT("Unknown");
        }
        return Type.GetName();
    }

    FString FormatPinType(const UEdGraphPin* Pin)
    {
        if (!Pin)
        {
            return TEXT("Unknown");
        }
        const FNiagaraTypeDefinition Type = UEdGraphSchema_Niagara::PinToTypeDefinition(Pin);
        if (Type.IsValid())
        {
            return FormatTypeName(Type);
        }
        if (const UObject* TypeObject = Pin->PinType.PinSubCategoryObject.Get())
        {
            const FString TypeObjectName = FormatTypeObjectName(TypeObject);
            if (!TypeObjectName.IsEmpty())
            {
                return TypeObjectName;
            }
        }
        return TEXT("Unknown");
    }

    FString FormatVersionSuffix(const FGuid& VersionGuid, const UNiagaraScript* Script)
    {
        if (VersionGuid == FGuid() || Script == nullptr)
        {
            return FString();
        }
        for (const FNiagaraAssetVersion& Version : Script->GetAllAvailableVersions())
        {
            if (Version.VersionGuid == VersionGuid)
            {
                return FString::Printf(TEXT("@v%d.%d"), Version.MajorVersion, Version.MinorVersion);
            }
        }
        return FString();
    }
}

namespace
{
    // Niagara pin defaults are stored as the serialised pin-default string (the
    // same string TrySetDefaultValue accepts and FNiagaraTypeDefinition::ToString
    // produces for each registered Niagara type). Most types emit verbatim from
    // Pin.DefaultValue; data-interface pins instead carry a class reference on
    // PinSubCategoryObject (no serialised default), and unlinked-empty defaults
    // surface as authoring gaps the caller should warn on.
    FString FormatNiagaraPinLiteral(const UEdGraphPin& Pin, FNIRTextEmitter& Out)
    {
        // Data-interface pin: PinSubCategoryObject resolves to a UClass deriving
        // from UNiagaraDataInterface. The literal carries no serialised default —
        // emit the class name as the value reference.
        if (UObject* SubObject = Pin.PinType.PinSubCategoryObject.Get())
        {
            if (UClass* SubClass = Cast<UClass>(SubObject))
            {
                static UClass* const NiagaraDataInterfaceClass = FindObject<UClass>(
                    nullptr, TEXT("/Script/Niagara.NiagaraDataInterface"));
                if (NiagaraDataInterfaceClass && SubClass->IsChildOf(NiagaraDataInterfaceClass))
                {
                    return FString::Printf(TEXT("dataInterface %s"), *SubClass->GetName());
                }
            }
        }

        // Empty default on an unlinked pin is an authoring gap (or an engine type
        // that doesn't serialise a default). Emit a marker and warn so the test
        // surfaces silent coverage holes.
        if (Pin.DefaultValue.IsEmpty() && Pin.LinkedTo.IsEmpty())
        {
            Out.Warn(FString::Printf(
                TEXT("FormatNiagaraPinLiteral: pin %s has empty default and no link; emitting <empty>."),
                *Pin.PinName.ToString()));
            return FString(TEXT("<empty>"));
        }

        return Pin.DefaultValue;
    }

    // Format the inner expression for a recursive dynamic-input call. Walks the
    // dynamic-input function-call's input pins and, for each pin that carries a
    // literal or wires upstream to another classified node, emits "input X = ...".
    // Returns the comma-separated body so the caller can wrap it in "dynamic ... { ... }".
    // Depth-cap enforcement lives in the caller (EmitInputValueExpr) — this helper
    // simply recurses one level deeper.
    FString EmitDynamicInputBody(UNiagaraNodeFunctionCall& DynamicInputNode, FNIRTextEmitter& Out, int32 Depth)
    {
        TArray<FString> InputClauses;
        for (UEdGraphPin* InputPin : DynamicInputNode.Pins)
        {
            if (!InputPin || InputPin->Direction != EGPD_Input)
            {
                continue;
            }
            // Skip the implicit parameter-map input pin if present (its PinName matches
            // the parameter-map type tag and it carries no caller-facing override).
            const FNiagaraTypeDefinition PinType = UEdGraphSchema_Niagara::PinToTypeDefinition(InputPin);
            if (PinType.IsValid() && PinType == FNiagaraTypeDefinition::GetParameterMapDef())
            {
                continue;
            }
            const FString Expr = EmitInputValueExpr(InputPin, Out, Depth + 1);
            if (Expr.IsEmpty())
            {
                continue;
            }
            InputClauses.Add(FString::Printf(
                TEXT("input %s = %s"),
                *NIRTextEmitter::FormatNameToken(InputPin->PinName.ToString()),
                *Expr));
        }
        return FString::Join(InputClauses, TEXT(", "));
    }
}

FString EmitInputValueExpr(UEdGraphPin* InputPin, FNIRTextEmitter& Out, int32 Depth)
{
    if (Depth >= GNIRMaxOverrideDepth)
    {
        Out.Warn(TEXT("EmitInputValueExpr: recursion limit (32) reached; truncated."));
        return FString(TEXT("# recursion-limit-reached"));
    }
    if (!InputPin)
    {
        return FString();
    }

    // Case 1: direct literal — no upstream link, pin carries its serialised default.
    if (InputPin->LinkedTo.IsEmpty())
    {
        return FormatNiagaraPinLiteral(*InputPin, Out);
    }

    // Find the single upstream output pin / node feeding this input pin.
    UEdGraphPin* UpstreamPin = InputPin->LinkedTo[0];
    UEdGraphNode* UpstreamNode = UpstreamPin ? UpstreamPin->GetOwningNode() : nullptr;
    if (!UpstreamNode)
    {
        return FormatNiagaraPinLiteral(*InputPin, Out);
    }

    // Case 2: UNiagaraNodeInput → linked parameter "$Namespace.Name".
    if (UNiagaraNodeInput* InputNode = Cast<UNiagaraNodeInput>(UpstreamNode))
    {
        return NIRTextEmitter::FormatParameterRef(InputNode->Input.GetName());
    }

    // Case 3: UNiagaraNodeFunctionCall → dynamic-input module call (recursive).
    if (UNiagaraNodeFunctionCall* FunctionCallNode = Cast<UNiagaraNodeFunctionCall>(UpstreamNode))
    {
        const FString ModuleName = FunctionCallNode->FunctionScript
            ? FunctionCallNode->FunctionScript->GetName()
            : FunctionCallNode->GetFunctionName();
        const FString VersionSuffix = NIRTextEmitter::FormatVersionSuffix(
            FunctionCallNode->SelectedScriptVersion,
            FunctionCallNode->FunctionScript);
        // Pass Depth (not Depth+1) — EmitDynamicInputBody increments Depth itself
        // when recursing through EmitInputValueExpr on inner pins, so adding +1 here
        // would double-count nesting levels and trip the depth-32 guard at chain
        // depth 16 instead of 32.
        const FString InnerBody = EmitDynamicInputBody(*FunctionCallNode, Out, Depth);
        if (InnerBody.IsEmpty())
        {
            return FString::Printf(TEXT("dynamic %s%s { }"),
                *NIRTextEmitter::FormatNameToken(ModuleName),
                *VersionSuffix);
        }
        return FString::Printf(TEXT("dynamic %s%s { %s }"),
            *NIRTextEmitter::FormatNameToken(ModuleName),
            *VersionSuffix,
            *InnerBody);
    }

    // Case 4 fallback: an unclassified upstream node. Emit the pin literal as a
    // best-effort approximation; the override is wired but not to a node class we
    // model. Warn so the dual-surface tests can flag silent coverage gaps.
    Out.Warn(FString::Printf(
        TEXT("EmitInputValueExpr: unclassified upstream node %s (class %s); emitting pin default."),
        *UpstreamNode->GetName(),
        *UpstreamNode->GetClass()->GetName()));
    return FormatNiagaraPinLiteral(*InputPin, Out);
}

FString FormatPinValueRef(UEdGraphPin* Pin)
{
    if (!Pin)
    {
        return FString();
    }
    if (!Pin->LinkedTo.IsEmpty())
    {
        UEdGraphPin* UpstreamPin = Pin->LinkedTo[0];
        UEdGraphNode* UpstreamNode = UpstreamPin ? UpstreamPin->GetOwningNode() : nullptr;
        if (UpstreamNode)
        {
            return FString::Printf(TEXT("%%%s.%s"),
                *UpstreamNode->GetName(),
                *UpstreamPin->PinName.ToString());
        }
    }
    // Verbatim-passthrough path: v1c graph-emitter callers don't carry an emitter
    // to warn through, and empty unlinked defaults are common on optional pins
    // (not authoring gaps). FormatNiagaraPinLiteral's warn-on-empty contract is
    // appropriate only for the override-chain (EmitInputValueExpr) callsite. Data-
    // interface pins are still rendered as "dataInterface ClassName" here because
    // their value lives on PinSubCategoryObject (a UClass), not in DefaultValue.
    if (UObject* SubObject = Pin->PinType.PinSubCategoryObject.Get())
    {
        if (UClass* SubClass = Cast<UClass>(SubObject))
        {
            static UClass* const NiagaraDataInterfaceClass = FindObject<UClass>(
                nullptr, TEXT("/Script/Niagara.NiagaraDataInterface"));
            if (NiagaraDataInterfaceClass && SubClass->IsChildOf(NiagaraDataInterfaceClass))
            {
                return FString::Printf(TEXT("dataInterface %s"), *SubClass->GetName());
            }
        }
    }
    return Pin->DefaultValue;
}

void EmitGraphBody(UNiagaraGraph* Graph, FNIRTextEmitter& Out)
{
    if (!Graph)
    {
        return;
    }
    const TMap<const UEdGraphNode*, FString> NodeNames = BuildGraphNodeNames(*Graph);
    EmitGraphNodeDeclarations(*Graph, NodeNames, Out);
    for (UEdGraphNode* EdNode : Graph->Nodes)
    {
        UNiagaraNode* Node = Cast<UNiagaraNode>(EdNode);
        if (!Node)
        {
            continue;
        }
        if (NIRGraphEmit_Dataflow(Node, Out))
        {
            continue;
        }
        if (NIRGraphEmit_Control(Node, Out))
        {
            continue;
        }
        if (NIRGraphEmit_Util(Node, Out))
        {
            continue;
        }
        const FString ClassName = Node->GetClass()->GetName();
        Out.AppendLine(FString::Printf(
            TEXT("# unknown-node %s%s"),
            *ClassName,
            *NIRTextEmitter::FormatPositionSuffix(Node)));
        Out.Warn(FString::Printf(
            TEXT("EmitGraphBody: unhandled Niagara node class %s; emitted unknown-node marker."),
            *ClassName));
    }
    EmitGraphLinks(*Graph, NodeNames, Out);
}

void EmitScriptGraphScope(const UNiagaraScript* Script, FNIRTextEmitter& Out)
{
    // fx.Niagara.OnDemandCompile is system-scoped; standalone-script emission deliberately omits the # compile-state-stale annotation.
    if (!Script)
    {
        return;
    }
    UNiagaraGraph* Graph = const_cast<UNiagaraGraph*>(NiagaraJsonHelpers::GetGraphFromScript(Script));
    const FString UsageName = NIRTextEmitter::FormatUsageName(Script->GetUsage());
    Out.EnterScope(FString::Printf(TEXT("graph %s "), *UsageName));
    EmitGraphBody(Graph, Out);
    Out.ExitScope();
}
