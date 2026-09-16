// Copyright (c) 2026 Alexander Penkin. MIT License.

// AGIRCompiler_CachedPose.cpp
//
// Wave 2 — save_cached_pose / use_cached_pose compile handlers. Cross-reference
// linkage is resolved through a block-scoped name map so a `use_cached_pose`
// can be linked back to its matching `save_cached_pose` editor node at AGIR
// compile time. Runtime FName fields (FAnimNode_SaveCachedPose::CachePoseName /
// FAnimNode_UseCachedPose::LinkToCachingNode) are intentionally not touched —
// the engine's OnProcessDuringCompilation populates them on AnimBP compile.

#include "AGIR/AGIRCliffHandlers.h"
#include "AGIR/AGIRCompilerHelpers.h"


#include "AnimGraphNode_Base.h"
#include "AnimGraphNode_SaveCachedPose.h"
#include "AnimGraphNode_UseCachedPose.h"
#include "Animation/AnimBlueprint.h"
#include "EdGraph/EdGraph.h"
#include "Handlers/Animation/AnimGraphConstructionUtils.h"
#include "IrCore/IrTextUtils.h"
#include "UObject/Class.h"
#include "UObject/UnrealType.h"

namespace
{
using AGIRCliff::Helpers::ApplyNodeGuidIfPresent;
using AGIRCliff::Helpers::FindArgValue;
using AGIRCliff::Helpers::IsPoseRefValue;
using AGIRCliff::Helpers::WriteUObjectFieldByName;

// Strip surrounding string-literal or name-token wrappers — `NameToken` in the
// emitter wraps cache-name values in quotes when they contain spaces, so the
// raw `name=...` value can arrive either bare or quoted.
FString UnquoteNameOrString(const FString& Value)
{
    FString Unwrapped;
    FString Error;
    if (FIrTextUtils::TryUnwrapStringLiteral(Value, Unwrapped, Error) ||
        FIrTextUtils::TryUnwrapNameToken(Value, Unwrapped, Error))
    {
        return Unwrapped;
    }
    return Value;
}
} // namespace

bool AGIRCliff::CompileSaveCachedPoseInstruction(
    UAnimBlueprint* /*AnimBP*/,
    UEdGraph* TargetGraph,
    const FAGIRInstruction& Inst,
    FAGIRSymbolMap& Symbols,
    FAGIRPendingPoseWires& PendingWires,
    FAGIRCacheNameMap& CacheNameMap,
    int32& OutNodesCreated,
    TArray<FString>& OutWarnings,
    FAGIRCompileResult& OutResult)
{
    const FVector2D Position = Inst.bHasPosition ? Inst.Position : FVector2D::ZeroVector;
    UAnimGraphNode_Base* NewBase = AnimGraphConstructionUtils::CreateAnimNode(
        TargetGraph, UAnimGraphNode_SaveCachedPose::StaticClass(), Position);
    UAnimGraphNode_SaveCachedPose* SaveNode = Cast<UAnimGraphNode_SaveCachedPose>(NewBase);
    if (!SaveNode)
    {
        OutResult = FAGIRCompileResult::MakeError(
            TEXT("AGIR_NODE_CREATE_FAILED"),
            FString::Printf(TEXT("Failed to create UAnimGraphNode_SaveCachedPose on graph '%s' (line %d)."),
                *TargetGraph->GetName(), Inst.SourceLine));
        return false;
    }
    ApplyNodeGuidIfPresent(SaveNode, Inst.NodeGuid);

    // Required `name=<cache>` arg. Empty / missing is an error — without a cache
    // name a use_cached_pose can't link back.
    const FString* NameValue = FindArgValue(Inst, TEXT("name"));
    if (!NameValue || NameValue->IsEmpty())
    {
        OutResult = FAGIRCompileResult::MakeError(
            TEXT("AGIR_BAD_OPCODE"),
            FString::Printf(TEXT("save_cached_pose missing 'name' arg (line %d)."),
                Inst.SourceLine));
        return false;
    }
    const FString CacheName = UnquoteNameOrString(*NameValue);
    SaveNode->CacheName = CacheName;

    if (!Inst.ResultName.IsEmpty())
    {
        Symbols.Add(Inst.ResultName, SaveNode);
    }
    CacheNameMap.Add(CacheName, SaveNode);

    // Walk remaining args. The pose input shows up as `Pose=%nN`; everything
    // else is a reflective field write that gets warned-but-tolerated, mirroring
    // the generic Call dispatch.
    for (const FAGIRArg& Arg : Inst.Args)
    {
        if (Arg.Name.IsEmpty() || Arg.Name == TEXT("name"))
        {
            continue;
        }
        if (IsPoseRefValue(Arg.Value))
        {
            FAGIRPendingPoseWire Wire;
            Wire.DownstreamNode = SaveNode;
            Wire.InputPinName = FName(*Arg.Name);
            Wire.UpstreamRef = Arg.Value;
            Wire.SourceLine = Inst.SourceLine;
            PendingWires.Add(MoveTemp(Wire));
            continue;
        }

        const FString WriteError = AGIRCliff::Helpers::WriteAnimNodeArg(
            SaveNode, FName(*Arg.Name), Arg.Value);
        if (!WriteError.IsEmpty())
        {
            OutWarnings.Add(FString::Printf(TEXT("AGIR_FIELD_WRITE: %s (line %d)"),
                *WriteError, Inst.SourceLine));
        }
    }

    ++OutNodesCreated;
    return true;
}

bool AGIRCliff::CompileUseCachedPoseInstruction(
    UAnimBlueprint* /*AnimBP*/,
    UEdGraph* TargetGraph,
    const FAGIRInstruction& Inst,
    FAGIRSymbolMap& Symbols,
    FAGIRPendingPoseWires& /*PendingWires*/,
    FAGIRPendingCacheLinks& PendingCacheLinks,
    int32& OutNodesCreated,
    TArray<FString>& OutWarnings,
    FAGIRCompileResult& OutResult)
{
    const FVector2D Position = Inst.bHasPosition ? Inst.Position : FVector2D::ZeroVector;
    UAnimGraphNode_Base* NewBase = AnimGraphConstructionUtils::CreateAnimNode(
        TargetGraph, UAnimGraphNode_UseCachedPose::StaticClass(), Position);
    UAnimGraphNode_UseCachedPose* UseNode = Cast<UAnimGraphNode_UseCachedPose>(NewBase);
    if (!UseNode)
    {
        OutResult = FAGIRCompileResult::MakeError(
            TEXT("AGIR_NODE_CREATE_FAILED"),
            FString::Printf(TEXT("Failed to create UAnimGraphNode_UseCachedPose on graph '%s' (line %d)."),
                *TargetGraph->GetName(), Inst.SourceLine));
        return false;
    }
    ApplyNodeGuidIfPresent(UseNode, Inst.NodeGuid);

    // Required `source=<cache>` arg.
    const FString* SourceValue = FindArgValue(Inst, TEXT("source"));
    if (!SourceValue || SourceValue->IsEmpty())
    {
        OutResult = FAGIRCompileResult::MakeError(
            TEXT("AGIR_BAD_OPCODE"),
            FString::Printf(TEXT("use_cached_pose missing 'source' arg (line %d)."),
                Inst.SourceLine));
        return false;
    }
    const FString CacheName = UnquoteNameOrString(*SourceValue);

    // NameOfCache is private mutable on the editor node; reflective write is the
    // only path. The engine's EarlyValidation re-resolves SaveCachedPoseNode
    // from this serialized name on the next AnimBP compile, but we also resolve
    // the linkage ourselves at AGIR compile time so AGIR-level assertions (and a
    // faithful warm re-decompile, which derives use_cached_pose's source= label
    // from SaveCachedPoseNode->CacheName) observe it without a full AnimBP
    // compile.
    const FString WriteError = WriteUObjectFieldByName(UseNode, FName(TEXT("NameOfCache")), CacheName);
    if (!WriteError.IsEmpty())
    {
        OutWarnings.Add(FString::Printf(TEXT("AGIR_FIELD_WRITE: %s (line %d)"),
            *WriteError, Inst.SourceLine));
    }

    // Defer SaveCachedPoseNode resolution to a post-loop Pass-2 (see
    // ResolvePendingCacheLinks in AGIRCompiler.cpp). On the canonical locomotion
    // emission order the use_cached_pose is compiled BEFORE its save_cached_pose
    // (a forward reference), so the save node is not yet in CacheNameMap here;
    // resolving against the completed map after every instruction in the block
    // compiles makes the linkage order-independent — mirroring how pose-pin
    // wires defer via FAGIRPendingPoseWire / ResolvePendingPoseWires.
    FAGIRPendingCacheLink PendingLink;
    PendingLink.UseNode = UseNode;
    PendingLink.CacheName = CacheName;
    PendingLink.SourceLine = Inst.SourceLine;
    PendingCacheLinks.Add(MoveTemp(PendingLink));

    if (!Inst.ResultName.IsEmpty())
    {
        Symbols.Add(Inst.ResultName, UseNode);
    }

    // Walk remaining args via reflection — UseCachedPose has no pose input pin
    // (it sources its pose from the linked save node), so no pose-wire queueing
    // happens here. Unknown reflected fields surface as warnings, matching the
    // generic Call dispatch.
    for (const FAGIRArg& Arg : Inst.Args)
    {
        if (Arg.Name.IsEmpty() || Arg.Name == TEXT("source"))
        {
            continue;
        }
        const FString FieldWriteError = AGIRCliff::Helpers::WriteAnimNodeArg(
            UseNode, FName(*Arg.Name), Arg.Value);
        if (!FieldWriteError.IsEmpty())
        {
            OutWarnings.Add(FString::Printf(TEXT("AGIR_FIELD_WRITE: %s (line %d)"),
                *FieldWriteError, Inst.SourceLine));
        }
    }

    ++OutNodesCreated;
    return true;
}
