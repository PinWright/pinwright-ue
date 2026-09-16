// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "AGIR/AGIRDecompiler.h"


#include "AGIR/AGIRGrammar.h"
#include "AGIR/AGIROpcodes.h"
#include "AGIR/AGIRTextEmitter.h"
#include "AnimGraphNode_LinkedInputPose.h"
#include "Handlers/Animation/AnimGraphConstructionUtils.h"

#include "Animation/AnimBlueprint.h"
#include "Animation/AnimBlueprintGeneratedClass.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"

namespace
{
using AnimGraphConstructionUtils::IsTopLevelAnimGraph;

// Walks the BP's implemented interfaces and filters the reachable graphs to
// the ones that classify as top-level anim graphs — anim layer interface
// override pose graphs. The shared `CollectInterfaceLayerGraphs` returns
// every reachable graph; AGIR only cares about the anim-layer subset.
void CollectAnimLayerGraphs(UBlueprint* Blueprint, TSet<const UEdGraph*>& OutLayerGraphs)
{
    TSet<const UEdGraph*> AllInterfaceGraphs;
    AnimGraphConstructionUtils::CollectInterfaceLayerGraphs(Blueprint, AllInterfaceGraphs);
    for (const UEdGraph* Graph : AllInterfaceGraphs)
    {
        if (IsTopLevelAnimGraph(Graph))
        {
            OutLayerGraphs.Add(Graph);
        }
    }
}

EAGIREntryKind ClassifyGraph(const UEdGraph* Graph, const TSet<const UEdGraph*>& LayerGraphs)
{
    // Anim layer interface override pose graphs are flagged via the BP's
    // `ImplementedInterfaces` walk. The main AnimGraph is canonically named
    // `UEdGraphSchema_K2::GN_AnimGraph` ("AnimGraph"). Anim function graphs
    // (anim BP functions that return a pose link) live alongside the main
    // graph on `FunctionGraphs` as `UAnimationGraph` instances, but are
    // distinguishable by name (anything other than "AnimGraph") AND by the
    // presence of at least one `UAnimGraphNode_LinkedInputPose` — that node
    // is the function-side counterpart of LinkedAnim and only appears inside
    // anim function bodies and anim layer implementation graphs.
    if (LayerGraphs.Contains(Graph))
    {
        return EAGIREntryKind::AnimLayer;
    }
    if (Graph && Graph->GetFName() != UEdGraphSchema_K2::GN_AnimGraph)
    {
        for (const UEdGraphNode* Node : Graph->Nodes)
        {
            if (Cast<const UAnimGraphNode_LinkedInputPose>(Node) != nullptr)
            {
                return EAGIREntryKind::AnimFunction;
            }
        }
    }
    return EAGIREntryKind::AnimGraph;
}

void SortGraphsByName(TArray<UEdGraph*>& Graphs)
{
    Graphs.Sort([](const UEdGraph& A, const UEdGraph& B)
    {
        return A.GetName().Compare(B.GetName(), ESearchCase::CaseSensitive) < 0;
    });
}
} // namespace

FAGIRDecompiler::FAGIRDecompiler(UAnimBlueprint* InAnimBP)
    : AnimBlueprint(InAnimBP)
{
}

FAGIRDecompileResult FAGIRDecompiler::Decompile()
{
    if (!AnimBlueprint)
    {
        return FAGIRDecompileResult::MakeError(TEXT("AGIR_NULL_BLUEPRINT"));
    }

    TSet<const UEdGraph*> LayerGraphs;
    CollectAnimLayerGraphs(AnimBlueprint, LayerGraphs);

    // Collect every top-level anim graph: the BP's FunctionGraphs filtered to
    // UAnimationGraph + interface-reachable layer graphs (some of which may
    // also be on FunctionGraphs; the TSet dedupes).
    TSet<const UEdGraph*> Seen;
    TArray<UEdGraph*> TopLevelGraphs;

    for (UEdGraph* Graph : AnimBlueprint->FunctionGraphs)
    {
        if (IsTopLevelAnimGraph(Graph) && !Seen.Contains(Graph))
        {
            Seen.Add(Graph);
            TopLevelGraphs.Add(Graph);
        }
    }
    for (const UEdGraph* LayerGraph : LayerGraphs)
    {
        if (LayerGraph && !Seen.Contains(LayerGraph))
        {
            Seen.Add(LayerGraph);
            TopLevelGraphs.Add(const_cast<UEdGraph*>(LayerGraph));
        }
    }

    SortGraphsByName(TopLevelGraphs);

    FAGIRDecompileResult Result;
    TArray<FString> Blocks;
    FAGIRTextEmitter Emitter(AnimBlueprint);

    // Sort classpaths lexicographically for diff-stable output.
    if (AnimBlueprint->ImplementedInterfaces.Num() > 0)
    {
        TArray<FString> InterfaceClassPaths;
        for (const FBPInterfaceDescription& InterfaceDesc : AnimBlueprint->ImplementedInterfaces)
        {
            const UClass* InterfaceClass = InterfaceDesc.Interface.Get();
            if (!InterfaceClass)
            {
                continue;
            }
            InterfaceClassPaths.Add(InterfaceClass->GetPathName());
        }
        if (InterfaceClassPaths.Num() > 0)
        {
            InterfaceClassPaths.Sort([](const FString& A, const FString& B)
            {
                return A.Compare(B, ESearchCase::CaseSensitive) < 0;
            });

            TArray<FString> ManifestLines;
            ManifestLines.Add(TEXT("interfaces {"));
            for (const FString& ClassPath : InterfaceClassPaths)
            {
                ManifestLines.Add(FString::Printf(TEXT("    implements `%s`"), *ClassPath));
            }
            ManifestLines.Add(TEXT("}"));
            Blocks.Add(FString::Join(ManifestLines, TEXT("\n")));
        }
    }

    for (UEdGraph* Graph : TopLevelGraphs)
    {
        const EAGIREntryKind Kind = ClassifyGraph(Graph, LayerGraphs);
        const FString GraphName = Graph->GetName();
        const FString EmittedText = Emitter.EmitGraph(Graph, Kind);
        if (EmittedText.IsEmpty())
        {
            Result.Warnings.Add(FString::Printf(
                TEXT("AGIR_EMIT_FAILED: graph '%s' produced empty text"),
                *GraphName));
            continue;
        }

        FString Block = FString::Printf(
            TEXT("# ==== Graph: %s (%s) ====\n"),
            *GraphName,
            EntryKindToText(Kind));
        Block += EmittedText;
        Blocks.Add(Block);
    }

    // Relay the emitter's lossy-read reports (a wired data pin whose driver AGIR
    // has no spelling for). Without them a reader cannot tell an incomplete text
    // from a complete-and-empty one.
    Result.Warnings.Append(Emitter.GetWarnings());

    // Child AnimBPs that don't redefine their own AnimGraph contribute zero
    // entries to TopLevelGraphs. If the parent is itself a generated AnimBP
    // class, emit a `delegates_to` stub so every AnimBP folder carries an AGIR
    // record (instead of asset.dump's empty-text guard silently dropping it).
    if (TopLevelGraphs.Num() == 0 && Blocks.Num() == 0)
    {
        if (UAnimBlueprintGeneratedClass* ParentAnimClass =
                Cast<UAnimBlueprintGeneratedClass>(AnimBlueprint->ParentClass))
        {
            TArray<FString> StubLines;
            StubLines.Add(TEXT("# ==== AnimBP delegates to parent ===="));
            StubLines.Add(FString::Printf(TEXT("delegates_to `%s`"),
                *ParentAnimClass->GetPathName()));
            Blocks.Add(FString::Join(StubLines, TEXT("\n")));
            Result.Warnings.Add(TEXT("AGIR_DELEGATES_TO_PARENT"));
        }
    }

    if (Blocks.Num() > 0)
    {
        Result.bSuccess = true;
        Result.AGIRText = FString::Join(Blocks, TEXT("\n\n"));
    }
    else if (TopLevelGraphs.Num() == 0)
    {
        // An anim BP that genuinely has no anim graphs and no AnimBP parent is
        // a valid (rare) state — surface it via a warning, not a failure.
        // Callers (asset.dump, anim.decompile_agir) skip empty AGIRText via
        // their own guards.
        Result.bSuccess = true;
        Result.AGIRText.Reset();
        Result.Warnings.Add(TEXT("Anim BP has no anim graphs."));
    }
    else
    {
        // The BP has anim graphs but every one of them failed to emit — this
        // is an actual decompile failure, not a no-content state. Warnings
        // already populated by the per-graph AGIR_EMIT_FAILED records above.
        Result.bSuccess = false;
    }

    return Result;
}
