// Copyright (c) 2026 Alexander Penkin. MIT License.

// AGIR layered_blend compile handler. Wave 2 implementation: instantiate a
// UAnimGraphNode_LayeredBoneBlend, grow its BlendPoses array to match the
// `BlendPoses_<i>=` arg count emitted by the decompiler, run ReconstructNode
// to materialise the editor pins, queue pose wires for Pass 2, and route any
// remaining reflected fields and data-pin bindings through WriteAnimNodeArg.

#include "AGIR/AGIRCliffHandlers.h"
#include "AGIR/AGIRCompilerHelpers.h"


#include "AGIR/AGIROpcodes.h"
#include "AGIR/AGIRParser.h"
#include "AnimGraphNode_Base.h"
#include "AnimGraphNode_LayeredBoneBlend.h"
#include "AnimNodes/AnimNode_LayeredBoneBlend.h"
#include "EdGraph/EdGraph.h"
#include "Handlers/Animation/AnimGraphConstructionUtils.h"

namespace
{
using AGIRCliff::Helpers::IsPoseRefValue;

// Match `BlendPoses_<digits>` and extract the numeric index. Returns
// INDEX_NONE for any other key. The decompiler emits these keys via the array
// pose-pin convention `<Field>_<Index>` (see AGIRTextEmitter::AppendPoseLinkFields).
int32 ParseBlendPoseIndex(const FString& ArgName)
{
    static const FString Prefix = TEXT("BlendPoses_");
    if (!ArgName.StartsWith(Prefix))
    {
        return INDEX_NONE;
    }
    const FString Tail = ArgName.Mid(Prefix.Len());
    if (Tail.IsEmpty() || !Tail.IsNumeric())
    {
        return INDEX_NONE;
    }
    return FCString::Atoi(*Tail);
}

}

bool AGIRCliff::CompileLayeredBlendInstruction(
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
            FString::Printf(TEXT("layered_blend target graph is null (line %d)."), Inst.SourceLine));
        return false;
    }

    // The `layered_blend` opcode keyword fixes the editor node class; AGIR's
    // SymbolName carries the source node's editor name (for round-trip
    // readability), not a class path. Use the StaticClass directly rather than
    // attempting LoadObject<UClass> on the node-name token.
    const FVector2D Position = Inst.bHasPosition ? Inst.Position : FVector2D::ZeroVector;
    UAnimGraphNode_Base* NewBase = AnimGraphConstructionUtils::CreateAnimNode(
        TargetGraph, UAnimGraphNode_LayeredBoneBlend::StaticClass(), Position);
    UAnimGraphNode_LayeredBoneBlend* NewNode = Cast<UAnimGraphNode_LayeredBoneBlend>(NewBase);
    if (!NewNode)
    {
        OutResult = FAGIRCompileResult::MakeError(
            TEXT("AGIR_NODE_CREATE_FAILED"),
            FString::Printf(TEXT("Failed to create layered_blend node on graph '%s' (line %d)."),
                *TargetGraph->GetName(), Inst.SourceLine));
        return false;
    }

    // NodeGuid override — apply before symbol publish so cross-graph references
    // (transition rule getters etc.) match the decompile-side guid.
    if (!Inst.NodeGuid.IsEmpty())
    {
        FGuid Parsed;
        if (FGuid::Parse(Inst.NodeGuid, Parsed))
        {
            NewNode->NodeGuid = Parsed;
        }
    }

    // Pass A: scan args to determine the required pose count from the highest
    // `BlendPoses_<i>=` key. The runtime constructor calls AddFirstPose() so
    // BlendPoses already has length 1 — only grow when the AGIR text references
    // index >= 1.
    int32 MaxBlendIndex = INDEX_NONE;
    for (const FAGIRArg& Arg : Inst.Args)
    {
        const int32 Index = ParseBlendPoseIndex(Arg.Name);
        if (Index != INDEX_NONE && Index > MaxBlendIndex)
        {
            MaxBlendIndex = Index;
        }
    }

    if (MaxBlendIndex >= 0)
    {
        const int32 RequiredCount = MaxBlendIndex + 1;
        while (NewNode->Node.BlendPoses.Num() < RequiredCount)
        {
            // Inlined FAnimNode_LayeredBoneBlend::AddPose(). The engine's
            // AddPose() is a header-inline that calls the private member
            // SyncBlendMasksAndLayers(); although the latter is tagged
            // ANIMGRAPHRUNTIME_API, the symbol is not exported from the
            // engine DLL in 5.6, producing an LNK2019 at link time. Inline
            // the entire chain using only public state (BlendWeights,
            // BlendPoses, BlendMode, BlendMasks, LayerSetup are all public
            // on FAnimNode_LayeredBoneBlend).
            NewNode->Node.BlendWeights.Add(1.f);
            NewNode->Node.BlendPoses.AddDefaulted();
            // Inlined SyncBlendMasksAndLayers().
            if (NewNode->Node.BlendMode == ELayeredBoneBlendMode::BlendMask)
            {
                NewNode->Node.BlendMasks.SetNum(NewNode->Node.BlendPoses.Num());
                NewNode->Node.LayerSetup.Reset();
            }
            else
            {
                NewNode->Node.BlendMasks.Reset();
                NewNode->Node.LayerSetup.SetNum(NewNode->Node.BlendPoses.Num());
            }
        }
        // Refresh editor pins so `BlendPoses_<i>` input pins materialise to
        // match the new array length. Without this, Pass-2 pose wiring fails
        // to find the per-index input pin.
        NewNode->ReconstructNode();
    }

    // Pass B: walk args. Pose-link values queue Pass-2 wires keyed by editor
    // pin name; everything else flows through reflective field write. Unknown
    // fields surface as warnings (mirrors the EAGIROpcode::Call arm).
    for (const FAGIRArg& Arg : Inst.Args)
    {
        if (Arg.Name.IsEmpty())
        {
            continue;
        }
        if (IsPoseRefValue(Arg.Value))
        {
            FAGIRPendingPoseWire Wire;
            Wire.DownstreamNode = NewNode;
            Wire.InputPinName = FName(*Arg.Name);
            Wire.UpstreamRef = Arg.Value;
            Wire.SourceLine = Inst.SourceLine;
            PendingWires.Add(MoveTemp(Wire));
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

    if (!Inst.ResultName.IsEmpty())
    {
        Symbols.Add(Inst.ResultName, NewNode);
    }

    ++OutNodesCreated;
    OutResult = FAGIRCompileResult();
    return true;
}
