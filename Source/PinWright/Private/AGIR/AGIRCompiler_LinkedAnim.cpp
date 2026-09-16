// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "AGIR/AGIRCliffHandlers.h"
#include "AGIR/AGIRCompilerHelpers.h"


#include "AnimGraphNode_LinkedAnimGraph.h"
#include "AnimGraphNode_LinkedAnimGraphBase.h"
#include "AnimGraphNode_LinkedAnimLayer.h"
#include "Animation/AnimBlueprint.h"
#include "EdGraph/EdGraph.h"
#include "Handlers/Animation/AnimGraphConstructionUtils.h"

namespace
{
using AGIRCliff::Helpers::IsPoseRefValue;

// Strip surrounding `"..."` if present so reflective writes through
// WriteAnimNodeArg do not double-quote FName payloads. Mirrors the
// helper in AGIRCompiler.cpp without taking a dependency on translation-unit
// scope.
FString UnquoteString(const FString& Value)
{
    if (Value.Len() >= 2 && Value.StartsWith(TEXT("\"")) && Value.EndsWith(TEXT("\"")))
    {
        return Value.Mid(1, Value.Len() - 2);
    }
    return Value;
}

// linked_anim leads with no name token (parser does not put anything in
// SymbolName for the LinkedAnim opcode), and the AGIR text grammar does not
// carry a class path — see AGIRTextEmitter::EmitLinkedAnim. Distinguish layer
// vs graph by presence of the `Layer=` arg: layers always emit a Layer name,
// LinkedAnimGraph nodes carry InstanceClass instead.
bool HasLayerArg(const FAGIRInstruction& Inst)
{
    for (const FAGIRArg& Arg : Inst.Args)
    {
        if (Arg.Name == TEXT("Layer"))
        {
            return true;
        }
    }
    return false;
}

}

bool AGIRCliff::CompileLinkedAnimInstruction(
    UAnimBlueprint* /*AnimBP*/,
    UEdGraph* TargetGraph,
    const FAGIRInstruction& Inst,
    FAGIRSymbolMap& Symbols,
    FAGIRPendingPoseWires& PendingWires,
    int32& OutNodesCreated,
    TArray<FString>& OutWarnings,
    FAGIRCompileResult& OutResult)
{
    if (!TargetGraph)
    {
        OutResult = FAGIRCompileResult::MakeError(
            TEXT("AGIR_TARGET_NOT_FOUND"),
            FString::Printf(TEXT("linked_anim has no target graph (line %d)."), Inst.SourceLine));
        return false;
    }

    UClass* NodeClass = HasLayerArg(Inst)
        ? UAnimGraphNode_LinkedAnimLayer::StaticClass()
        : UAnimGraphNode_LinkedAnimGraph::StaticClass();

    const FVector2D Position = Inst.bHasPosition ? Inst.Position : FVector2D::ZeroVector;
    UAnimGraphNode_Base* NewNode = AnimGraphConstructionUtils::CreateAnimNode(
        TargetGraph, NodeClass, Position);
    if (!NewNode)
    {
        OutResult = FAGIRCompileResult::MakeError(
            TEXT("AGIR_NODE_CREATE_FAILED"),
            FString::Printf(TEXT("Failed to create %s on graph '%s' (line %d)."),
                *NodeClass->GetName(), *TargetGraph->GetName(), Inst.SourceLine));
        return false;
    }

    if (!Inst.NodeGuid.IsEmpty())
    {
        FGuid Parsed;
        if (FGuid::Parse(Inst.NodeGuid, Parsed))
        {
            NewNode->NodeGuid = Parsed;
        }
    }

    // First pass over args: write reflective fields on the runtime FAnimNode_*
    // struct (Layer / InstanceClass / bReceiveNotifies* etc.). Pose inputs
    // (`<ParamName>=%nN`) are deferred to Pass 2 after ReconstructNode() has
    // materialised the per-input pins from the layer's UFunction signature.
    UAnimGraphNode_LinkedAnimLayer* AsLayer = Cast<UAnimGraphNode_LinkedAnimLayer>(NewNode);
    for (const FAGIRArg& Arg : Inst.Args)
    {
        if (Arg.Name.IsEmpty())
        {
            continue;
        }
        if (IsPoseRefValue(Arg.Value))
        {
            // Defer pose wiring until pins exist.
            continue;
        }

        if (AsLayer && Arg.Name == TEXT("Layer"))
        {
            // SetupFromLayerId writes Node.Layer + Node.Interface + InterfaceGuid
            // + FunctionReference in one call so the layer node is fully wired
            // back to its interface declaration. Bypassing this would leave
            // Node.Interface null and ReconstructNode would synthesise no
            // input-pose pins for layer parameters.
            AnimGraphConstructionUtils::SetupLinkedAnimLayerFromLayerId(
                AsLayer, FName(*UnquoteString(Arg.Value)));
            continue;
        }

        const FString WriteError = AGIRCliff::Helpers::WriteAnimNodeArg(
            NewNode, FName(*Arg.Name), Arg.Value);
        if (!WriteError.IsEmpty())
        {
            OutWarnings.Add(FString::Printf(TEXT("AGIR_FIELD_WRITE: %s (line %d)"),
                *WriteError, Inst.SourceLine));
        }
    }

    // ReconstructNode rebuilds the editor pins from the now-populated runtime
    // struct; for LinkedAnimLayer this derives input-pose pins from the layer
    // interface UFunction's pose-typed parameters. Required before queueing
    // pose wires so Pass 2 finds the matching pin names.
    NewNode->ReconstructNode();

    // Second arg pass: queue pose-wire defers. Names match the layer
    // UFunction parameter names (e.g. `MyInputPose`); the resolver looks them
    // up by FName on the editor node's pin set.
    for (const FAGIRArg& Arg : Inst.Args)
    {
        if (Arg.Name.IsEmpty() || !IsPoseRefValue(Arg.Value))
        {
            continue;
        }
        FAGIRPendingPoseWire Wire;
        Wire.DownstreamNode = NewNode;
        Wire.InputPinName = FName(*Arg.Name);
        Wire.UpstreamRef = Arg.Value;
        Wire.SourceLine = Inst.SourceLine;
        PendingWires.Add(MoveTemp(Wire));
    }

    if (!Inst.ResultName.IsEmpty())
    {
        Symbols.Add(Inst.ResultName, NewNode);
    }
    ++OutNodesCreated;

    OutResult = FAGIRCompileResult();
    return true;
}
