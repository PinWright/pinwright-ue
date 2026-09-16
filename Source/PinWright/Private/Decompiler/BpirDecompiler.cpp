// Copyright (c) 2026 Alexander Penkin. MIT License.

// BpirDecompiler.cpp - Decompiles Blueprint graphs into BPIR text format

#include "Decompiler/BpirDecompiler.h"
#include "Compiler/BpirSharedConstants.h"
#include "Compiler/BpirStructLiteralUtils.h"
#include "Compiler/CodeNodeEmitter.h"
#include "Decompiler/AnimGraphFamilyCheck.h"
#include "Decompiler/BpirInputKeyHelpers.h"
#include "Decompiler/GraphWalker.h"
#include "Handlers/Blueprint/BlueprintHandlerUtils.h"
#include "Decompiler/BpirTextEmitter.h"
#include "Decompiler/DecompilerTypes.h"
#include "IrCore/IrTextUtils.h"
#include "Engine/Blueprint.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_MacroInstance.h"
#include "K2Node_DynamicCast.h"
#include "K2Node_Timeline.h"
#include "K2Node_CallFunction.h"
#include "K2Node_VariableSet.h"
#include "K2Node_VariableGet.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_Event.h"
#include "K2Node_ExecutionSequence.h"
#include "K2Node_Switch.h"
#include "K2Node_Self.h"
#include "K2Node_Knot.h"
#include "K2Node_AsyncAction.h"
#include "K2Node_InputKey.h"
#include "EdGraphNode_Comment.h"
#include "K2Node_MakeStruct.h"
#include "K2Node_BreakStruct.h"
#include "K2Node_Tunnel.h"
#include "K2Node_Composite.h"
#include "EdGraphSchema_K2.h"
#include "EdGraphUtilities.h"
#include "Internationalization/Text.h"
#include "Compat/EngineVersionCompat.h"
#include "UObject/Package.h"
#include "UObject/StructOnScope.h"

#if __has_include("K2Node_CallDelegate.h")
#include "K2Node_CallDelegate.h"
#include "K2Node_AddDelegate.h"
#include "K2Node_RemoveDelegate.h"
#include "K2Node_ClearDelegate.h"
#define MCP_HAS_DELEGATE_NODES 1
#else
#define MCP_HAS_DELEGATE_NODES 0
#endif

#if __has_include("K2Node_BaseMCDelegate.h")
#include "K2Node_BaseMCDelegate.h"
#endif

#if __has_include("K2Node_ComponentBoundEvent.h")
#include "K2Node_ComponentBoundEvent.h"
#define MCP_HAS_COMPONENT_BOUND_EVENT 1
#else
#define MCP_HAS_COMPONENT_BOUND_EVENT 0
#endif

#if __has_include("WidgetBlueprint.h")
#include "WidgetBlueprint.h"
#include "Blueprint/WidgetTree.h"
#define MCP_HAS_WIDGET_BLUEPRINT 1
#else
#define MCP_HAS_WIDGET_BLUEPRINT 0
#endif

DEFINE_LOG_CATEGORY(LogBpirDecompiler);

namespace BpirDecompiler::Helpers
{
    // Walks backward through chained UK2Node_Knot reroute nodes starting from a data
    // SourcePin (the result of `Pin->LinkedTo[0]` on a non-knot input). Returns the
    // upstream pin whose owning node is no longer a knot (the real source), OR a pin
    // owned by a knot when the chain dead-ends at a knot whose KnotInput has no link
    // (caller can read that knot's KnotInput default value), OR nullptr when the
    // chain breaks entirely. OutDeadEndKnotInput receives the KnotInput pin of the
    // terminal knot in the dead-end case so the caller can recurse on its default.
    UEdGraphPin* FollowKnotsBackward(UEdGraphPin* SourcePin, UEdGraphPin*& OutDeadEndKnotInput)
    {
        OutDeadEndKnotInput = nullptr;
        UEdGraphNode* SourceNode = SourcePin ? SourcePin->GetOwningNode() : nullptr;
        // Cap traversal to bound malformed graphs with cyclic knot routing.
        int32 Hops = 0;
        while (SourceNode && SourceNode->IsA<UK2Node_Knot>())
        {
            if (++Hops > 1024) return nullptr;
            UEdGraphPin* KnotInput = nullptr;
            for (UEdGraphPin* Pin : SourceNode->Pins)
            {
                if (Pin && Pin->Direction == EGPD_Input
                    && Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec)
                {
                    KnotInput = Pin;
                    break;
                }
            }
            if (KnotInput && KnotInput->LinkedTo.Num() > 0)
            {
                SourcePin = KnotInput->LinkedTo[0];
                SourceNode = SourcePin ? SourcePin->GetOwningNode() : nullptr;
            }
            else
            {
                OutDeadEndKnotInput = KnotInput;
                return nullptr;
            }
        }
        return SourcePin;
    }

    bool NodeSupportsMultiSelf(UEdGraphNode* Node)
    {
        if (!Node)
        {
            return false;
        }
        if (UK2Node_CallFunction* CallNode = Cast<UK2Node_CallFunction>(Node))
        {
            return CallNode->AllowMultipleSelfs(/*bInputAsArray=*/false);
        }
#if __has_include("K2Node_BaseMCDelegate.h")
        if (Node->IsA<UK2Node_BaseMCDelegate>())
        {
            return true;
        }
#endif
        return false;
    }

}

namespace
{
    // Bound on split nesting; a struct can only be split as deep as it nests, so
    // this only guards against a malformed ParentPin cycle.
    constexpr int32 MaxSplitPinDepth = 64;

    using FOrphanNodeKey = BlueprintHandlerUtils::FBlueprintOrphanNodeKey;

    TArray<UEdGraph*> CollectBpirTopLevelGraphs(UBlueprint* Blueprint)
    {
        TArray<UEdGraph*> Result;
        if (!Blueprint)
        {
            return Result;
        }

        TSet<const UEdGraph*> Seen;
        auto AppendUnique = [&Result, &Seen](UEdGraph* Graph)
        {
            if (Graph && !Seen.Contains(Graph))
            {
                Seen.Add(Graph);
                Result.Add(Graph);
            }
        };

        for (UEdGraph* Graph : Blueprint->UbergraphPages)
        {
            AppendUnique(Graph);
        }
        for (UEdGraph* Graph : Blueprint->FunctionGraphs)
        {
            AppendUnique(Graph);
        }
        for (UEdGraph* Graph : Blueprint->MacroGraphs)
        {
            AppendUnique(Graph);
        }
        for (const FBPInterfaceDescription& Interface : Blueprint->ImplementedInterfaces)
        {
            for (UEdGraph* Graph : Interface.Graphs)
            {
                AppendUnique(Graph);
            }
        }
        return Result;
    }

    bool IsImplementedInterfaceGraph(const UBlueprint* Blueprint, const UEdGraph* Graph)
    {
        if (!Blueprint || !Graph)
        {
            return false;
        }
        for (const FBPInterfaceDescription& Interface : Blueprint->ImplementedInterfaces)
        {
            for (UEdGraph* InterfaceGraph : Interface.Graphs)
            {
                if (InterfaceGraph == Graph)
                {
                    return true;
                }
            }
        }
        return false;
    }

    // Clone/flatten is an emission-only view. Pair nodes one-to-one within their
    // corresponding authored graph before flattening so duplicate GUIDs in sibling
    // graphs never fall through a global GUID lookup.
    void MapGraphNodesToSource(
        UEdGraph* SourceGraph,
        UEdGraph* WorkingGraph,
        TMap<const UEdGraphNode*, FOrphanNodeKey>& OutSourceKeys)
    {
        if (!SourceGraph || !WorkingGraph)
        {
            return;
        }

        // The GUID index is local to this one graph and only pairs clone nodes with
        // their authored counterpart; all orphan decisions still use FOrphanNodeKey.
        TMap<FGuid, TArray<UEdGraphNode*>> SourceNodesByGuid;
        for (UEdGraphNode* SourceNode : SourceGraph->Nodes)
        {
            if (SourceNode && SourceNode->NodeGuid.IsValid())
            {
                SourceNodesByGuid.FindOrAdd(SourceNode->NodeGuid).Add(SourceNode);
            }
        }
        TMap<FGuid, int32> NextSourceIndexByGuid;
        for (UEdGraphNode* WorkingNode : WorkingGraph->Nodes)
        {
            if (!WorkingNode || !WorkingNode->NodeGuid.IsValid())
            {
                continue;
            }

            const TArray<UEdGraphNode*>* SourceCandidates =
                SourceNodesByGuid.Find(WorkingNode->NodeGuid);
            if (!SourceCandidates)
            {
                continue;
            }

            int32& NextSourceIndex = NextSourceIndexByGuid.FindOrAdd(WorkingNode->NodeGuid);
            if (!SourceCandidates->IsValidIndex(NextSourceIndex))
            {
                continue;
            }

            UEdGraphNode* MatchedSourceNode = (*SourceCandidates)[NextSourceIndex++];
            if (MatchedSourceNode)
            {
                OutSourceKeys.Add(
                    WorkingNode,
                    FOrphanNodeKey{SourceGraph, MatchedSourceNode->NodeGuid});
            }
        }
    }

    void MapWorkingGraphFamilyToSource(
        UEdGraph* SourceRoot,
        UEdGraph* WorkingRoot,
        TMap<const UEdGraphNode*, FOrphanNodeKey>& OutSourceKeys)
    {
        MapGraphNodesToSource(SourceRoot, WorkingRoot, OutSourceKeys);
        if (!SourceRoot || !WorkingRoot)
        {
            return;
        }

        // GetAllChildrenGraphs already includes all descendants. Pair each returned family
        // list once so nested graphs are not repeatedly remapped during recursive descent.
        TArray<UEdGraph*> SourceChildren;
        SourceRoot->GetAllChildrenGraphs(SourceChildren);
        TArray<UEdGraph*> WorkingChildren;
        WorkingRoot->GetAllChildrenGraphs(WorkingChildren);

        const int32 GraphPairCount = FMath::Min(SourceChildren.Num(), WorkingChildren.Num());
        for (int32 GraphIndex = 0; GraphIndex < GraphPairCount; ++GraphIndex)
        {
            MapGraphNodesToSource(SourceChildren[GraphIndex], WorkingChildren[GraphIndex], OutSourceKeys);
        }
    }

    // A split struct pin is the graph's *inline* struct break: UEdGraphSchema_K2::SplitPin
    // hides the struct pin, creates one sub-pin per member named
    // "<ParentPinName>_<MemberName>", and points each sub-pin's ParentPin back at it.
    // The value the node actually produces still lives on the parent, so every value
    // reference must be resolved against the split root and the members re-attached as a
    // dotted access.
    UEdGraphPin* GetSplitPinRoot(UEdGraphPin* Pin)
    {
        int32 Hops = 0;
        while (Pin && Pin->ParentPin && ++Hops <= MaxSplitPinDepth)
        {
            Pin = Pin->ParentPin;
        }
        return Pin;
    }

    // Dotted ".Member" (or ".Outer.Inner" for a nested split) suffix from the split root
    // down to SourcePin, each segment formatted as a BPIR name token. Empty when SourcePin
    // is not a split sub-pin, so non-split callers keep their existing spelling.
    //
    // The member half of a sub-pin name is the break node's member / out-param name
    // verbatim — the same spelling FBpirValueResolver's chain walker re-resolves, so
    // `$Hit.HitBoneName` and `%n0.OutHit.BoneName` recompile through the auto-break path.
    // Rendering the root alone is what let two members of one struct collapse onto the same
    // text (every sub-pin of a variable read printed as the bare `$Var`), which is how a
    // wire off FHitResult's `BoneName` (the *tracing* component's bone) passed review as a
    // read of `HitBoneName` (the bone that was hit).
    FString FormatSplitMemberSuffix(UEdGraphPin* SourcePin)
    {
        TArray<FString> Members;
        int32 Hops = 0;
        for (UEdGraphPin* Current = SourcePin;
             Current && Current->ParentPin && ++Hops <= MaxSplitPinDepth;
             Current = Current->ParentPin)
        {
            const FString ChildName = Current->PinName.ToString();
            const FString Prefix = Current->ParentPin->PinName.ToString() + TEXT("_");
            const FString Member = ChildName.StartsWith(Prefix, ESearchCase::CaseSensitive)
                ? ChildName.RightChop(Prefix.Len())
                : ChildName;
            Members.Insert(FIrTextUtils::FormatNameToken(Member), 0);
        }

        if (Members.Num() == 0)
        {
            return FString();
        }
        return FString(TEXT(".")) + FString::Join(Members, TEXT("."));
    }

    // Format an entry-point output pin (function-entry, event/customevent param,
    // or macro-tunnel-entry param) as a $-prefixed BPIR identifier. Returns an
    // empty string when the pin is not a formattable parameter pin (wrong
    // direction or exec category). Caller is responsible for the node-class
    // predicate that decides which branch this applies to.
    FString TryFormatEntryParam(UEdGraphPin* SourcePin)
    {
        if (!SourcePin || SourcePin->Direction != EGPD_Output)
        {
            return FString();
        }
        if (SourcePin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
        {
            return FString();
        }
        UEdGraphPin* const RootPin = GetSplitPinRoot(SourcePin);
        const FString Name = FIrTextUtils::FormatNameToken(RootPin->PinName.ToString());
        return FString::Printf(TEXT("$%s%s"), *Name, *FormatSplitMemberSuffix(SourcePin));
    }

    FString FormatOutputPinNameToken(UEdGraphPin* SourcePin)
    {
        if (!SourcePin)
        {
            return FString();
        }

        FString PinName = SourcePin->PinName.ToString();
        if (Cast<UK2Node_DynamicCast>(SourcePin->GetOwningNode())
            && SourcePin->Direction == EGPD_Output
            && SourcePin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec
            && SourcePin->PinType.PinCategory != UEdGraphSchema_K2::PC_Boolean)
        {
            PinName.ReplaceInline(TEXT(" "), TEXT(""));
        }
        return FIrTextUtils::FormatNameToken(PinName);
    }

    using BpirAnimGraphFamily::IsAnimGraphFamily;

    // Returns the target node's input exec pin name when the target has >=2
    // input exec pins (Gate / DoOnce / MultiGate / multi-tunnel macros).
    // Returns empty for the dominant single-input case so the emitter omits
    // the dotted ".PinName" suffix and existing fixtures don't churn.
    FString GetTargetInputPinNameIfMultiInput(UEdGraphPin* SourceExecOut)
    {
        if (!SourceExecOut || SourceExecOut->LinkedTo.Num() == 0) return FString();
        UEdGraphPin* TargetIn = SourceExecOut->LinkedTo[0];
        if (!TargetIn || !TargetIn->GetOwningNode()) return FString();
        int32 ExecInputCount = 0;
        for (UEdGraphPin* P : TargetIn->GetOwningNode()->Pins)
        {
            if (P && P->Direction == EGPD_Input
                && P->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
            {
                if (++ExecInputCount >= 2) break;
            }
        }
        return ExecInputCount >= 2 ? TargetIn->PinName.ToString() : FString();
    }

    FString FormatExecGotoLine(const FString& Label, UEdGraphPin* SourceExecOut)
    {
        const FString LabelTok = FIrTextUtils::FormatNameToken(Label);
        const FString TargetInputPinName = GetTargetInputPinNameIfMultiInput(SourceExecOut);
        if (TargetInputPinName.IsEmpty())
        {
            return FString::Printf(TEXT("    exec -> @%s"), *LabelTok);
        }

        const FString PinTok = FIrTextUtils::FormatNameToken(TargetInputPinName);
        return FString::Printf(TEXT("    exec -> @%s.%s"), *LabelTok, *PinTok);
    }

    bool IsSharedExecInputPin(UEdGraphPin* Pin)
    {
        return Pin
            && Pin->Direction == EGPD_Input
            && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec
            && Pin->LinkedTo.Num() > 1;
    }

    // Replace any non-identifier character with '_' so the result is a safe
    // NSLOCTEXT namespace/key segment (NSLOCTEXT identifiers must be ASCII
    // word characters; the F-require-ftext-localization-identity gate parses
    // them through FTextStringHelper::CreateFromBuffer, which is strict about
    // unescaped punctuation inside the namespace/key tokens).
    FString SanitizeLocIdentSegment(const FString& In)
    {
        FString Out;
        Out.Reserve(In.Len());
        for (TCHAR Ch : In)
        {
            const bool bAlnum = (Ch >= TEXT('A') && Ch <= TEXT('Z'))
                || (Ch >= TEXT('a') && Ch <= TEXT('z'))
                || (Ch >= TEXT('0') && Ch <= TEXT('9'))
                || Ch == TEXT('_');
            Out.AppendChar(bAlnum ? Ch : TEXT('_'));
        }
        return Out;
    }

    // Build a deterministic NSLOCTEXT identity for an FText pin default that
    // arrived from the asset with no namespace/key. Namespace is the owning
    // asset's short name (sanitized); Key is the first 8 hex chars of the
    // owning node's GUID followed by '.' and the sanitized pin name. Both
    // halves are stable across re-decompiles of the same Blueprint, so the
    // BPIR round-trips idempotently through the F-require-ftext-localization-
    // identity compile gate without manual hand-editing.
    FString SynthesizeNSLocTextLiteral(const UEdGraphPin* InputPin, const UBlueprint* OwningBlueprint, const FString& DisplayString)
    {
        const FString AssetName = OwningBlueprint ? OwningBlueprint->GetName() : FString(TEXT("UnknownAsset"));
        FString Namespace = SanitizeLocIdentSegment(AssetName);
        if (Namespace.IsEmpty())
        {
            Namespace = TEXT("BpirSynth");
        }

        const UEdGraphNode* OwningNode = InputPin ? InputPin->GetOwningNode() : nullptr;
        FString GuidPrefix;
        if (OwningNode)
        {
            const FString GuidDigits = OwningNode->NodeGuid.ToString(EGuidFormats::Digits);
            GuidPrefix = GuidDigits.Left(8);
        }
        if (GuidPrefix.IsEmpty())
        {
            GuidPrefix = TEXT("00000000");
        }

        const FString PinSegment = SanitizeLocIdentSegment(InputPin ? InputPin->PinName.ToString() : FString());
        const FString Key = PinSegment.IsEmpty()
            ? GuidPrefix
            : FString::Printf(TEXT("%s.%s"), *GuidPrefix, *PinSegment);

        return FString::Printf(
            TEXT("NSLOCTEXT(\"%s\", \"%s\", \"%s\")"),
            *BpirStructLiteralUtils::EscapeBpirStringInner(Namespace),
            *BpirStructLiteralUtils::EscapeBpirStringInner(Key),
            *BpirStructLiteralUtils::EscapeBpirStringInner(DisplayString));
    }
}

// ---------------------------------------------------------------------------
// FEntryState helpers
// ---------------------------------------------------------------------------

FString FBpirDecompiler::FEntryState::AllocValueName()
{
    return FIrTextUtils::FormatNumericLocalId(NextValueIndex++);
}

FString FBpirDecompiler::FEntryState::AllocLabel(const FString& Preferred)
{
    FString Candidate = Preferred;
    if (UsedLabels.Contains(Candidate))
    {
        // Append suffix to avoid collision
        int32 Suffix = 2;
        do
        {
            Candidate = FString::Printf(TEXT("%s_%d"), *Preferred, Suffix++);
        }
        while (UsedLabels.Contains(Candidate));
    }
    UsedLabels.Add(Candidate);
    return Candidate;
}

void FBpirDecompiler::AppendNodeLine(FEntryState& State, UEdGraphNode* Node, const FString& Line)
{
    const int32 GeneratedPrefixLineCount = TextEmitter
        ? TextEmitter->ConsumeGeneratedPrefixLineCount()
        : 0;
    const int32 FirstLineIndex = State.Lines.Num();
    if (!Line.Contains(TEXT("\n")))
    {
        const FString EnabledState = FBpirTextEmitter::GetNodeEnabledStateMarker(Node);
        const FString EnabledStateSuffix = EnabledState.IsEmpty()
            ? FString()
            : FString::Printf(TEXT(" %s"), *EnabledState);
        State.Lines.Add(FString::Printf(
            TEXT("    %s%s @(%d, %d)"),
            *Line,
            *EnabledStateSuffix,
            Node ? Node->NodePosX : 0,
            Node ? Node->NodePosY : 0));
        if (Node)
        {
            State.NodeToLineIndex.Add(Node, FirstLineIndex);
        }
        return;
    }

    TArray<FString> EmittedLines;
    Line.ParseIntoArrayLines(EmittedLines, /*bCullEmpty=*/false);
    if (EmittedLines.Num() == 0)
    {
        EmittedLines.Add(FString());
    }

    // Generated pure helpers use the same column and vertical cadence as the
    // compiler's ordinary pure-node placement (CodeNodeEmitter.cpp). Only the
    // explicitly reported prefix lines are helpers; every following line is an
    // authored consumer statement and keeps its exact position. Keep the map at
    // FirstLineIndex so reconvergence labels wrap the whole generated prefix.
    constexpr int32 GeneratedNodeHorizontalStep = 300;
    constexpr int32 GeneratedNodeInitialYOffset = 80;
    constexpr int32 GeneratedNodeVerticalStep = 130;
    const int32 ClampedGeneratedPrefixLineCount = FMath::Clamp(
        GeneratedPrefixLineCount, 0, EmittedLines.Num());
    const int32 ConsumerX = Node ? Node->NodePosX : 0;
    const int32 ConsumerY = Node ? Node->NodePosY : 0;

    for (int32 LineIndex = 0; LineIndex < EmittedLines.Num(); ++LineIndex)
    {
        const bool bIsGeneratedLine = LineIndex < ClampedGeneratedPrefixLineCount;
        const int32 PositionX = !bIsGeneratedLine || !Node
            ? ConsumerX
            : ConsumerX - GeneratedNodeHorizontalStep;
        const int32 PositionY = !bIsGeneratedLine || !Node
            ? ConsumerY
            : ConsumerY + GeneratedNodeInitialYOffset
                + (LineIndex * GeneratedNodeVerticalStep);
        FString EmittedLine = EmittedLines[LineIndex];
        if (LineIndex == EmittedLines.Num() - 1)
        {
            const FString EnabledState = FBpirTextEmitter::GetNodeEnabledStateMarker(Node);
            if (!EnabledState.IsEmpty())
            {
                EmittedLine += FString::Printf(TEXT(" %s"), *EnabledState);
            }
        }
        State.Lines.Add(FString::Printf(
            TEXT("    %s @(%d, %d)"),
            *EmittedLine,
            PositionX,
            PositionY));
    }
    if (Node)
    {
        State.NodeToLineIndex.Add(Node, FirstLineIndex);
    }
}

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

FBpirDecompiler::FBpirDecompiler(UBlueprint* InTargetBlueprint)
    : TargetBlueprint(InTargetBlueprint)
{
    TextEmitter = MakeUnique<FBpirTextEmitter>();
}

FBpirDecompiler::~FBpirDecompiler() = default;

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

FBpirDecompileResult FBpirDecompiler::Decompile()
{
    if (!TargetBlueprint)
    {
        return FBpirDecompileResult::MakeError(TEXT("Decompile: TargetBlueprint is null"));
    }

    TArray<FString> AllEntryTexts;
    TArray<FBpirWarning> AllWarnings;

    // Warn about graph types we don't decompile
    if (TargetBlueprint->DelegateSignatureGraphs.Num() > 0)
    {
        AllWarnings.Add(FBpirWarning{FString::Printf(
            TEXT("Skipped %d delegate signature graph(s) (not supported by decompiler)"),
            TargetBlueprint->DelegateSignatureGraphs.Num())});
    }
    // Aggregate decompile spans multiple graphs; the per-graph EmptyReason is collapsed
    // by the dump builder's single-graph path (BuildBpirText calls DecompileGraph/Function/Macro
    // per graph). Here we throw the per-graph reason away — Result.EmptyReason stays NotEmpty
    // since the joined BpirText covers many graphs at once.
    EBpirEmptyReason DiscardedReason = EBpirEmptyReason::NotEmpty;
    for (UEdGraph* Graph : CollectBpirTopLevelGraphs(TargetBlueprint))
    {
        if (IsAnimGraphFamily(Graph))
        {
            AllWarnings.Add(FBpirWarning{FString::Printf(
                TEXT("Skipped anim-graph %s — use AGIR (anim.decompile_agir) instead"),
                *Graph->GetName())});
            continue;
        }
        DecompileGraphInternal(Graph, AllEntryTexts, AllWarnings, DiscardedReason);
    }

    FBpirDecompileResult Result;
    Result.bSuccess = true;
    Result.BpirText = FString::Join(AllEntryTexts, TEXT("\n\n"));
    Result.Warnings = MoveTemp(AllWarnings);
    return Result;
}

FBpirDecompileResult FBpirDecompiler::DecompileGraph(const FString& GraphName)
{
    if (!TargetBlueprint)
    {
        return FBpirDecompileResult::MakeError(TEXT("DecompileGraph: TargetBlueprint is null"));
    }

    UEdGraph* Found = nullptr;
    for (UEdGraph* Graph : CollectBpirTopLevelGraphs(TargetBlueprint))
    {
        if (Graph->GetName() == GraphName)
        {
            Found = Graph;
            break;
        }
    }

    if (!Found)
    {
        return FBpirDecompileResult::MakeError(
            FString::Printf(TEXT("DecompileGraph: graph '%s' not found"), *GraphName));
    }

    TArray<FString> EntryTexts;
    TArray<FBpirWarning> Warnings;
    EBpirEmptyReason EmptyReason = EBpirEmptyReason::NotEmpty;
    DecompileGraphInternal(Found, EntryTexts, Warnings, EmptyReason);

    FBpirDecompileResult Result;
    Result.bSuccess = true;
    Result.BpirText = FString::Join(EntryTexts, TEXT("\n\n"));
    Result.Warnings = MoveTemp(Warnings);
    Result.EmptyReason = Result.BpirText.IsEmpty() ? EmptyReason : EBpirEmptyReason::NotEmpty;
    return Result;
}

FBpirDecompileResult FBpirDecompiler::DecompileFunction(const FString& FunctionName)
{
    if (!TargetBlueprint)
    {
        return FBpirDecompileResult::MakeError(TEXT("DecompileFunction: TargetBlueprint is null"));
    }

    UEdGraph* Found = nullptr;
    for (UEdGraph* G : TargetBlueprint->FunctionGraphs)
    {
        if (G && G->GetName() == FunctionName)
        {
            Found = G;
            break;
        }
    }
    if (!Found)
    {
        for (const FBPInterfaceDescription& Interface : TargetBlueprint->ImplementedInterfaces)
        {
            for (UEdGraph* Graph : Interface.Graphs)
            {
                if (Graph && Graph->GetName() == FunctionName)
                {
                    Found = Graph;
                    break;
                }
            }
            if (Found)
            {
                break;
            }
        }
    }

    if (!Found)
    {
        return FBpirDecompileResult::MakeError(
            FString::Printf(TEXT("DecompileFunction: function '%s' not found"), *FunctionName));
    }

    TArray<FString> EntryTexts;
    TArray<FBpirWarning> Warnings;
    EBpirEmptyReason EmptyReason = EBpirEmptyReason::NotEmpty;
    DecompileGraphInternal(Found, EntryTexts, Warnings, EmptyReason);

    FBpirDecompileResult Result;
    Result.bSuccess = true;
    Result.BpirText = FString::Join(EntryTexts, TEXT("\n\n"));
    Result.Warnings = MoveTemp(Warnings);
    Result.EmptyReason = Result.BpirText.IsEmpty() ? EmptyReason : EBpirEmptyReason::NotEmpty;
    return Result;
}

FBpirDecompileResult FBpirDecompiler::DecompileMacro(const FString& MacroName)
{
    if (!TargetBlueprint)
    {
        return FBpirDecompileResult::MakeError(TEXT("DecompileMacro: TargetBlueprint is null"));
    }

    UEdGraph* Found = nullptr;
    for (UEdGraph* G : TargetBlueprint->MacroGraphs)
    {
        if (G && G->GetName() == MacroName)
        {
            Found = G;
            break;
        }
    }

    if (!Found)
    {
        return FBpirDecompileResult::MakeError(
            FString::Printf(TEXT("DecompileMacro: macro '%s' not found"), *MacroName));
    }

    TArray<FString> EntryTexts;
    TArray<FBpirWarning> Warnings;
    EBpirEmptyReason EmptyReason = EBpirEmptyReason::NotEmpty;
    DecompileGraphInternal(Found, EntryTexts, Warnings, EmptyReason);

    FBpirDecompileResult Result;
    Result.bSuccess = true;
    Result.BpirText = FString::Join(EntryTexts, TEXT("\n\n"));
    Result.Warnings = MoveTemp(Warnings);
    Result.EmptyReason = Result.BpirText.IsEmpty() ? EmptyReason : EBpirEmptyReason::NotEmpty;
    return Result;
}

// ---------------------------------------------------------------------------
// Internal: per-graph decompilation
// ---------------------------------------------------------------------------

void FBpirDecompiler::DecompileGraphInternal(
    UEdGraph* Graph,
    TArray<FString>& OutEntryTexts,
    TArray<FBpirWarning>& OutWarnings,
    EBpirEmptyReason& OutEmptyReason)
{
    if (!Graph)
    {
        return;
    }

    // Captured before any cloning: emit paths that surface the graph name (function /
    // override / macro entry signatures, cast diagnostics) read this instead of the
    // working graph's own name.
    CurrentSourceGraphName = Graph->GetName();
    const bool bIsInterfaceGraph = IsImplementedInterfaceGraph(TargetBlueprint, Graph);

    // Orphan warnings use the authored graph family, which is also the view scanned by
    // blueprint.graph.find_orphaned_nodes. The emitter may flatten a clone below, but
    // that presentation-only operation must not change reachability or graph identity.
    TArray<UEdGraph*> OrphanModelGraphs;
    BlueprintHandlerUtils::CollectBlueprintOrphanGraphFamily(Graph, OrphanModelGraphs);
    const BlueprintHandlerUtils::FBlueprintOrphanReachability OrphanModel =
        BlueprintHandlerUtils::BuildBlueprintOrphanReachability(
            OrphanModelGraphs, /*bIncludeDataOnly=*/true);

    // K2Node_Composite is editor-only — the engine compiler dissolves it before any
    // node-handler scheduling — and round-trip identity for composites is not a goal.
    // Pre-flatten every composite (recursive, depth-first) so the rest of the decompiler
    // never sees a UK2Node_Composite.
    //
    // Cloning has costs: FEdGraphUtilities::CloneGraph (via DuplicateObject) auto-suffixes
    // the clone's name when the outer has a sibling with the same name, yielding e.g.
    // "Set Error_2" instead of "Set Error" — which is why CurrentSourceGraphName above
    // is threaded to the emitter rather than deriving names from the working graph.
    // Skip cloning when the graph contains no composites — the overwhelming majority of
    // decompile targets — so we only pay for the clone when there's a composite to flatten.
    auto GraphHasComposite = [](UEdGraph* G) -> bool
    {
        if (!G) return false;
        for (UEdGraphNode* Node : G->Nodes)
        {
            if (Node && Node->IsA<UK2Node_Composite>()) return true;
        }
        return false;
    };

    UEdGraph* WorkingGraph = Graph;
    if (GraphHasComposite(Graph))
    {
        // Outer must be the source graph's UBlueprint (not GetTransientPackage()): several
        // engine node methods called during the walk — UK2Node_BaseAsyncTask::GetFactoryFunction,
        // UK2Node_Composite::DestroyNode, etc. — call FindBlueprintForNodeChecked() and assert
        // when the outer chain has no UBlueprint. Same outer choice
        // FKismetCompilerContext::ProcessOneFunctionGraph makes for its own clone.
        UObject* CloneOuter = Graph->GetTypedOuter<UBlueprint>();
        if (!CloneOuter)
        {
            CloneOuter = GetTransientPackage();
        }
        UEdGraph* Clone = FEdGraphUtilities::CloneGraph(
            Graph, CloneOuter, /*MessageLog=*/nullptr, /*bCloningForCompile=*/false);
        if (Clone)
        {
            WorkingGraph = Clone;
        }
    }

    // Preserve exact authored graph identity for the emission-only working view. This
    // map is intentionally keyed by the cloned node pointer; it is never used to classify
    // authored orphan warnings and has no ambiguous GUID fallback.
    TMap<const UEdGraphNode*, FOrphanNodeKey> WorkingNodeSourceKeys;
    MapWorkingGraphFamilyToSource(Graph, WorkingGraph, WorkingNodeSourceKeys);

    if (WorkingGraph != Graph)
    {
        InlineCompositesInPlace(WorkingGraph, OutWarnings);
    }

    // Fresh walker per graph
    GraphWalker = MakeUnique<FGraphWalker>(WorkingGraph);

    TArray<UEdGraphNode*> EntryPoints = GraphWalker->FindEntryPoints();

    // Filter out empty event entry points that are auto-created skeletons.
    // - FunctionEntry nodes are always kept even when empty (including the auto-generated
    //   UserConstructionScript) so the decompiler emits their signature as `entry ... {}`.
    //   This keeps the empty-graph representation canonical and round-trippable.
    // - Event/CustomEvent nodes with no body are kept (user may intentionally create them).
    // - When the graph has a mix of connected and unconnected events, drop the unconnected ones
    //   (they're default stubs like the auto-created BeginPlay).
    auto HasConnectedExec = [](UEdGraphNode* Node) -> bool {
        for (UEdGraphPin* Pin : Node->Pins)
        {
            if (Pin && Pin->Direction == EGPD_Output
                && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec
                && Pin->LinkedTo.Num() > 0)
            {
                return true;
            }
        }
        return false;
    };

    // For event nodes: only remove unconnected ones when connected ones exist in the same graph
    bool bAnyConnectedEvent = false;
    for (UEdGraphNode* Node : EntryPoints)
    {
        if (HasConnectedExec(Node)) { bAnyConnectedEvent = true; break; }
    }

    if (bAnyConnectedEvent)
    {
        EntryPoints.RemoveAll([&HasConnectedExec](UEdGraphNode* Node) {
            return !HasConnectedExec(Node);
        });
    }

    // Per-entry rendered state, kept alive past the emission loop so a post-pass
    // can append orphan pure nodes (e.g. an unwired `%ss = subsystem<X>()` whose
    // result is never consumed) into whichever entry block we choose to host them.
    struct FRenderedEntry
    {
        UEdGraphNode* EntryNode = nullptr;
        FEntryState State;
    };
    TArray<FRenderedEntry> RenderedEntries;
    RenderedEntries.Reserve(EntryPoints.Num());

    for (UEdGraphNode* EntryNode : EntryPoints)
    {
        if (!EntryNode)
        {
            continue;
        }

        FString Signature = TextEmitter->EmitEntrySignature(
            EntryNode,
            TargetBlueprint,
            CurrentSourceGraphName,
            bIsInterfaceGraph);

        // AnimGraph roots (and any other node class the engine root-sets) reach
        // FindEntryPoints through the class-agnostic backstop in IsBlueprintEntryNode,
        // but the grammar has no signature for them. Rendering one produced an anonymous
        // `entry event UnknownEntry() {}` stub — 24 of them for ABP_Manny's AnimGraph,
        // which carries its authored state in anim_graph.json / agir.txt instead. Skip
        // the ones the emitter cannot name and that gate no exec body; a graph left with
        // no entries falls through to the empty-graph classification below.
        if (Signature == FBpirTextEmitter::UnknownEntrySignature && !HasConnectedExec(EntryNode))
        {
            continue;
        }

        RenderedEntries.Emplace();
        FRenderedEntry& Rendered = RenderedEntries.Last();
        Rendered.EntryNode = EntryNode;
        FEntryState& State = Rendered.State;

        // Emit entry signature (with optional @meta / @flags decorator lines above it)
        for (const FString& DecoratorLine : TextEmitter->EmitEntryDecoratorLines(EntryNode))
        {
            State.Lines.Add(DecoratorLine);
        }
        State.Lines.Add(Signature + TEXT(" {"));

        // Validate ComponentBoundEvent bindings (BndEvt nodes) to catch broken widget delegate bindings.
        // When widgets are recreated, the BndEvt graph node survives but the widget-level binding breaks.
#if MCP_HAS_COMPONENT_BOUND_EVENT
        if (UK2Node_ComponentBoundEvent* CompEvent = Cast<UK2Node_ComponentBoundEvent>(EntryNode))
        {
            const FString CompName = CompEvent->ComponentPropertyName.ToString();
            const FString EventName = CompEvent->DelegatePropertyName.ToString();

            bool bComponentFound = false;
            FString BoundEntryKind = TEXT("component_event");
            FString MissingSubject = TEXT("component");

#if MCP_HAS_WIDGET_BLUEPRINT
            if (UWidgetBlueprint* WidgetBP = Cast<UWidgetBlueprint>(TargetBlueprint))
            {
                BoundEntryKind = TEXT("widget_event");
                MissingSubject = TEXT("widget");
                // Widget BP: check WidgetTree for the named widget variable
                if (WidgetBP->WidgetTree)
                {
                    bComponentFound = (WidgetBP->WidgetTree->FindWidget(CompEvent->ComponentPropertyName) != nullptr);
                }
            }
            else
#endif // MCP_HAS_WIDGET_BLUEPRINT
            {
                // Actor/non-widget BP: check GeneratedClass for an FObjectProperty matching the component name
                if (TargetBlueprint->GeneratedClass)
                {
                    bComponentFound = (FindFProperty<FObjectProperty>(TargetBlueprint->GeneratedClass, CompEvent->ComponentPropertyName) != nullptr);
                }
            }

            if (!bComponentFound)
            {
                State.Warnings.Add(FBpirWarning{FString::Printf(
                    TEXT("%s %s.%s: %s variable '%s' not found - it may have been removed or recreated without recompiling"),
                    *BoundEntryKind, *CompName, *EventName, *MissingSubject, *CompName)});
            }
            else if (!(TargetBlueprint->GeneratedClass
                && FindFProperty<FObjectProperty>(TargetBlueprint->GeneratedClass, CompEvent->ComponentPropertyName)
                && CompEvent->GetTargetDelegateProperty()))
            {
                State.Warnings.Add(FBpirWarning{FString::Printf(
                    TEXT("%s %s.%s: delegate binding is invalid - the delegate may no longer exist on the component class"),
                    *BoundEntryKind, *CompName, *EventName)});
            }
        }
#endif // MCP_HAS_COMPONENT_BOUND_EVENT

        // Mark entry node as visited
        State.VisitedNodes.Add(EntryNode);

        // Find the exec output pin to start walking.
        UEdGraphPin* ExecStartPin = GraphWalker->GetExecOutputPin(EntryNode, 0);
        if (UClass* EnhancedInputNodeClass = FCodeNodeEmitter::ResolveEnhancedInputActionNodeClass();
            EnhancedInputNodeClass && EntryNode->IsA(EnhancedInputNodeClass))
        {
            TArray<UEdGraphPin*> ConnectedEventPins;
            for (const FName EventPinName : FCodeNodeEmitter::GetEnhancedInputActionEventPinNames())
            {
                UEdGraphPin* EventPin = EntryNode->FindPin(EventPinName, EGPD_Output);
                if (EventPin && EventPin->LinkedTo.Num() > 0)
                {
                    ConnectedEventPins.Add(EventPin);
                }
            }

            if (ConnectedEventPins.Num() == 1
                && ConnectedEventPins[0]->PinName == TEXT("Triggered"))
            {
                ExecStartPin = ConnectedEventPins[0];
            }
            else
            {
                ExecStartPin = nullptr;
                for (UEdGraphPin* EventPin : ConnectedEventPins)
                {
                    const FString Label = State.AllocLabel(EventPin->PinName.ToString().ToLower());
                    State.PendingLabels.Add({Label, EventPin});
                }
            }

            if (!FCodeNodeEmitter::GetEnhancedInputAction(EntryNode))
            {
                State.Warnings.Add(FBpirWarning{
                    TEXT("Unbound K2Node_EnhancedInputAction cannot be round-tripped")});
            }
        }
        if (UK2Node_InputKey* InputKeyNode = Cast<UK2Node_InputKey>(EntryNode))
        {
            if (FBpirInputKeyHelpers::IsInputKeyExecPinActive(InputKeyNode, /*bReleased=*/true))
            {
                ExecStartPin = FBpirInputKeyHelpers::FindInputKeyExecPin(InputKeyNode, /*bReleased=*/true);
            }
        }

        // Walk the initial exec chain (pre-label block, CurrentWalkLabel is empty)
        State.CurrentWalkLabel = FString();
        WalkExecChain(ExecStartPin, State);

        // Process queued labels
        int32 LabelIdx = 0;
        while (LabelIdx < State.PendingLabels.Num())
        {
            TPair<FString, UEdGraphPin*> LabelEntry = State.PendingLabels[LabelIdx++];

            State.Lines.Add(FString());
            State.Lines.Add(FString::Printf(TEXT("@%s:"), *LabelEntry.Key));

            State.CurrentWalkLabel = LabelEntry.Key;
            WalkExecChain(LabelEntry.Value, State);
        }

        // Pure macro support: if this entry is a TunnelEntry, check for an unvisited
        // exit tunnel. Pure macros have no exec pins, so the exec walk above produces
        // nothing. We trace backward from the exit tunnel's data inputs to discover
        // and emit pure dependency nodes, then emit the macro return line.
        if (GraphWalker->ClassifyNode(EntryNode) == ENodeSemantics::TunnelEntry)
        {
            for (UEdGraphNode* GNode : WorkingGraph->Nodes)
            {
                UK2Node_Tunnel* ExitTunnel = Cast<UK2Node_Tunnel>(GNode);
                if (!ExitTunnel || !ExitTunnel->bCanHaveInputs || ExitTunnel->bCanHaveOutputs)
                {
                    continue;
                }

                if (State.VisitedNodes.Contains(ExitTunnel))
                {
                    continue;
                }

                // Unvisited exit tunnel found — pure macro case
                State.VisitedNodes.Add(ExitTunnel);

                auto ResolvePin = [this, &State](UEdGraphPin* Pin) -> FString
                {
                    return ResolveInputValue(Pin, State);
                };

                EmitPureDependencies(ExitTunnel, State);

                FString Line = TextEmitter->EmitMacroReturn(ExitTunnel, ResolvePin);
                AppendNodeLine(State, ExitTunnel, Line);
                break; // Only one exit tunnel per macro graph
            }
        }

        // Closing brace is appended later, after the orphan-pure injection pass below,
        // so injected statements land inside the entry block.
    }

    // Pure orphans that feed a downstream consumer are deliberately not hoisted. The
    // shared model's data closure makes the same nodes warning candidates below.

    // Inject orphan pure named-result nodes (e.g. an unwired `%ss = subsystem<X>()`
    // statement authored for side-effect with no downstream consumer) into the first
    // event-kind entry so they survive a decompile -> recompile round-trip. Without
    // this, pure orphans drop on the floor because the exec-walk never reaches them
    // and EmitPureDependencies only emits demand-driven dependencies of impure nodes.
    //
    // Only GENUINELY STANDALONE pure orphans (no downstream consumer) are hoisted. A
    // pure orphan whose output is wired to other nodes is a data producer for that
    // (entry-less) subgraph; grafting it into an unrelated reachable entry's body would
    // be misleading and non-round-trippable, so it is omitted and warned instead.
    {
        FRenderedEntry* HostEntry = nullptr;
        for (FRenderedEntry& Candidate : RenderedEntries)
        {
            if (Candidate.EntryNode && Candidate.EntryNode->IsA<UK2Node_Event>())
            {
                HostEntry = &Candidate;
                break;
            }
        }
        if (!HostEntry && RenderedEntries.Num() > 0)
        {
            HostEntry = &RenderedEntries[0];
        }

        if (HostEntry)
        {
            // Build the union of nodes already visited by any entry's walk so we don't
            // re-emit them.
            TSet<UEdGraphNode*> GloballyVisited;
            for (const FRenderedEntry& R : RenderedEntries)
            {
                for (UEdGraphNode* N : R.State.VisitedNodes)
                {
                    GloballyVisited.Add(N);
                }
            }

            FEntryState& State = HostEntry->State;
            auto ResolvePin = [this, &State](UEdGraphPin* InPin) -> FString
            {
                return ResolveInputValue(InPin, State);
            };

            for (UEdGraphNode* Node : WorkingGraph->Nodes)
            {
                if (!Node) continue;
                if (GloballyVisited.Contains(Node)) continue;
                if (BlueprintHandlerUtils::IsBlueprintEntryNode(Node)) continue;
                if (Node->IsA<UK2Node_Knot>()) continue;
                if (Node->IsA<UEdGraphNode_Comment>()) continue;
                // Inlined pure helpers must not surface as standalone statements.
                if (Node->IsA<UK2Node_Self>()) continue;
                if (Node->IsA<UK2Node_VariableGet>()) continue;

                const FOrphanNodeKey* SourceKey = WorkingNodeSourceKeys.Find(Node);
                const BlueprintHandlerUtils::FBlueprintOrphanNodeShape* Shape = SourceKey
                    ? OrphanModel.NodeShapes.Find(*SourceKey)
                    : nullptr;
                bool bHasExecPin = Shape && Shape->bHasExecPins;
                if (!Shape)
                {
                    // Presentation-only nodes that have no authored source counterpart
                    // cannot participate in orphan policy; inspect only their shape.
                    for (const UEdGraphPin* Pin : Node->Pins)
                    {
                        if (Pin && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
                        {
                            bHasExecPin = true;
                            break;
                        }
                    }
                }
                if (bHasExecPin) continue;

                // A pure orphan that feeds other nodes is a data producer for an
                // entry-less subgraph (e.g. an auto-play Timeline's VLerp / GetWorldLocation
                // feeders), not a standalone side-effect statement. Hoisting it into the
                // host entry's body would falsely attribute it to that entry and break the
                // round-trip, so omit it; the orphan-warning sweep below flags it instead.
                if (!SourceKey || !OrphanModel.StandalonePureNodes.Contains(*SourceKey))
                {
                    continue;
                }

                // Mark visited first so any pure dependency walk doesn't redundantly
                // re-emit this node when chasing its data inputs.
                State.VisitedNodes.Add(Node);
                EmitPureDependencies(Node, State);

                const FString ValueName = State.AllocValueName();
                State.NodeToValueName.Add(Node, ValueName);
                const FString Line = TextEmitter->EmitPureNode(Node, ValueName, ResolvePin);
                AppendNodeLine(State, Node, Line);
            }
        }
    }

    // Close each entry block and serialize.
    for (FRenderedEntry& Rendered : RenderedEntries)
    {
        Rendered.State.Lines.Add(TEXT("}"));
        OutEntryTexts.Add(FString::Join(Rendered.State.Lines, TEXT("\n")));
        OutWarnings.Append(Rendered.State.Warnings);
    }

    // Do not consult WalkExecChain's State.VisitedNodes here — knots are not recorded there.
    // Finder/deletion use the same shared IsOrphan decision.
    for (UEdGraph* SourceGraph : OrphanModelGraphs)
    {
        if (!SourceGraph) continue;

        for (UEdGraphNode* Node : SourceGraph->Nodes)
        {
            if (!OrphanModel.IsOrphan(Node, /*bIncludeDataOnly=*/true)) continue;

            // Use FullTitle so user-facing functions render with display names
            // (e.g. "Print String" not "PrintString"); strip context/RPC lines
            // that FullTitle appends so the warning stays single-line.
            FString FriendlyTitle = Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString();
            int32 NewlineIdx = INDEX_NONE;
            if (FriendlyTitle.FindChar(TEXT('\n'), NewlineIdx))
            {
                FriendlyTitle = FriendlyTitle.Left(NewlineIdx);
            }

            OutWarnings.Add(FBpirWarning{FString::Printf(
                TEXT("Orphaned node not reachable from any entry point: %s :: %s '%s' nodeId=%s @(%d,%d)"),
                *SourceGraph->GetName(),
                *Node->GetClass()->GetName(),
                *FriendlyTitle,
                *Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens),
                Node->NodePosX,
                Node->NodePosY)});
        }
    }

    // Post-pass: remove empty auto-generated event entries when non-empty entries exist.
    // Preserves intentionally empty events when they're the only entries in the graph.
    // Never removes custom_event entries — those are always user-created.
    if (OutEntryTexts.Num() > 1)
    {
        auto IsEmptyEntry = [](const FString& Text) -> bool {
            // Empty entry: just "entry ... {\n}" with no body lines
            int32 NewlineCount = 0;
            for (TCHAR Ch : Text)
            {
                if (Ch == TEXT('\n')) { ++NewlineCount; }
            }
            return NewlineCount <= 1; // signature+"{" \n "}" = 1 newline
        };

        // Only remove empty entries if at least one non-empty entry exists
        bool bHasNonEmpty = false;
        for (const FString& Text : OutEntryTexts)
        {
            if (!IsEmptyEntry(Text)) { bHasNonEmpty = true; break; }
        }

        if (bHasNonEmpty)
        {
            OutEntryTexts.RemoveAll([&IsEmptyEntry](const FString& Text) {
                // Never remove custom_event entries — they are always intentional
                if (Text.Contains(TEXT("entry custom_event")))
                {
                    return false;
                }
                return IsEmptyEntry(Text);
            });
        }
    }

    // Classify why the per-graph BPIR ended up empty so the dump builder can pick the
    // matching `# (...)` marker. NotEmpty stays the default when entries were emitted.
    // Every recognized entry point now renders as an `entry ... {}` block, so empty
    // BpirText with a non-zero node count means the graph exposed no entry point the
    // walker could seed from — UnreachableGraph.
    if (OutEntryTexts.Num() == 0)
    {
        OutEmptyReason = EBpirEmptyReason::UnreachableGraph;
        // AssetDumpBuilder overrides to ZeroNodes when Graph->Nodes is empty.
    }
}

// ---------------------------------------------------------------------------
// Composite inline pre-pass
// ---------------------------------------------------------------------------

void FBpirDecompiler::InlineCompositesInPlace(UEdGraph* Graph, TArray<FBpirWarning>& OutWarnings, int32 Depth)
{
    if (!Graph)
    {
        return;
    }
    // Bound recursion depth to defend against pathologically nested composite graphs
    // (or any cycle in BoundGraph references that survives cloning).
    if (Depth > 1024)
    {
        OutWarnings.Add(FBpirWarning{TEXT("Composite inline depth exceeded 1024 — aborting flatten pass for this graph"), EBpirWarningSeverity::Error});
        return;
    }

    const UEdGraphSchema_K2* K2Schema = GetDefault<UEdGraphSchema_K2>();

    // Iterate by index since we mutate Graph->Nodes (composites move their children in,
    // then we remove the composite + its boundary tunnels). Each surviving composite is
    // recursively flattened first so nested composites collapse from the inside out.
    for (int32 NodeIdx = 0; NodeIdx < Graph->Nodes.Num(); )
    {
        UK2Node_Composite* Composite = Cast<UK2Node_Composite>(Graph->Nodes[NodeIdx]);
        if (!Composite || !Composite->BoundGraph)
        {
            ++NodeIdx;
            continue;
        }

        UEdGraph* BoundGraph = Composite->BoundGraph;

        // Depth-first: flatten any composites nested inside BoundGraph before we move
        // its contents into the parent.
        InlineCompositesInPlace(BoundGraph, OutWarnings, Depth + 1);

        UK2Node_Tunnel* InEntry = Composite->InputSinkNode;
        UK2Node_Tunnel* InResult = Composite->OutputSourceNode;

        // Move all nodes from BoundGraph into the parent graph so CollapseGatewayNode
        // can reconnect pins without crossing graph boundaries. This includes the
        // entry/exit tunnels themselves (we destroy them after the collapse).
        BoundGraph->MoveNodesToAnotherGraph(Graph, /*bIsLoading=*/false, /*bInIsCompiling=*/true);

        const bool bCollapsed = K2Schema->CollapseGatewayNode(
            Composite, InEntry, InResult, /*CompilerContext=*/nullptr, /*OutExpandedNodes=*/nullptr);
        if (!bCollapsed)
        {
            OutWarnings.Add(FBpirWarning{FString::Printf(
                TEXT("Composite '%s' had un-twinned boundary pins during inline; partial flatten applied"),
                BoundGraph ? *BoundGraph->GetName() : TEXT("?"))});
        }

        // UK2Node_Composite::DestroyNode asserts via FindBlueprintForNodeChecked when
        // the outer graph has no UBlueprint owner — true for our transient clone. Tear
        // down manually: break links, drop from Nodes, mark garbage. The clone itself
        // is collected when DecompileGraphInternal returns.
        auto TearDownNode = [Graph](UEdGraphNode* Node)
        {
            if (!Node) return;
            Node->BreakAllNodeLinks();
            Graph->Nodes.Remove(Node);
            Node->MarkAsGarbage();
        };
        TearDownNode(InEntry);
        if (InResult != InEntry)
        {
            TearDownNode(InResult);
        }
        TearDownNode(Composite);

        // SubGraphs may still reference BoundGraph (CloneGraph copied the pointer);
        // remove it so the orphan-warning sweep doesn't re-walk an empty subgraph.
        Graph->SubGraphs.Remove(BoundGraph);

        // Restart at NodeIdx — moved-in inner nodes occupy fresh slots and one of
        // them might itself be a composite (in pathological hand-built fixtures);
        // re-scanning from the same index covers that without an extra outer loop.
    }
}

// ---------------------------------------------------------------------------
// Core exec chain walker
// ---------------------------------------------------------------------------

void FBpirDecompiler::WalkExecChain(UEdGraphPin* ExecPin, FEntryState& State)
{
    if (!ExecPin)
    {
        return;
    }

    UEdGraphPin* CurrentExecPin = ExecPin;
    bool bEmittedExecutableNode = false;

    while (CurrentExecPin && CurrentExecPin->LinkedTo.Num() > 0)
    {
        if (CurrentExecPin->LinkedTo.Num() > 1)
        {
            State.Warnings.Add(FBpirWarning{FString::Printf(
                TEXT("Exec pin '%s' has %d connections but only the first is followed (BPIR constraint)"),
                *CurrentExecPin->PinName.ToString(), CurrentExecPin->LinkedTo.Num())});
        }

        UEdGraphPin* LinkedPin = CurrentExecPin->LinkedTo[0];
        if (!LinkedPin)
        {
            break;
        }
        UEdGraphNode* Node = LinkedPin->GetOwningNode();
        if (!Node)
        {
            break;
        }

        // Transparently skip reroute (knot) nodes — follow through to their output
        if (Node->IsA<UK2Node_Knot>())
        {
            UEdGraphPin* KnotOut = GraphWalker->GetExecOutputPin(Node, 0);
            if (KnotOut)
            {
                CurrentExecPin = KnotOut;
                continue;
            }
            break;
        }

        ENodeSemantics Semantics = GraphWalker->ClassifyNode(Node);
        auto MakeExecTarget = [&State](const FString& PreferredLabel, UEdGraphPin* SourceExecOut) -> FBpirEmitTarget
        {
            if (SourceExecOut && SourceExecOut->LinkedTo.Num() > 0)
            {
                UEdGraphPin* TargetPin = SourceExecOut->LinkedTo[0];
                UEdGraphNode* TargetNode = TargetPin ? TargetPin->GetOwningNode() : nullptr;
                // UK2Node_MacroInstance derives from UK2Node_Tunnel but is a legitimate
                // shared-exec target (Gate.Open, MultiGate, etc.); only exclude pure tunnel
                // entry/exit nodes from the merge-label logic.
                const bool bIsPureTunnel = TargetNode
                    && TargetNode->IsA<UK2Node_Tunnel>()
                    && !TargetNode->IsA<UK2Node_MacroInstance>();
                if (TargetNode && !bIsPureTunnel && IsSharedExecInputPin(TargetPin))
                {
                    FString MergeLabel;
                    if (FString* ExistingLabel = State.NodeToEmittedLabel.Find(TargetNode))
                    {
                        MergeLabel = *ExistingLabel;
                    }
                    if (MergeLabel.IsEmpty())
                    {
                        MergeLabel = State.AllocLabel(TEXT("merge"));
                        State.NodeToEmittedLabel.Add(TargetNode, MergeLabel);
                        State.PendingLabels.Add({MergeLabel, SourceExecOut});
                    }
                    return FBpirEmitTarget{MergeLabel, GetTargetInputPinNameIfMultiInput(SourceExecOut)};
                }
            }

            const FString Label = State.AllocLabel(PreferredLabel);
            State.PendingLabels.Add({Label, SourceExecOut});
            return FBpirEmitTarget{Label, GetTargetInputPinNameIfMultiInput(SourceExecOut)};
        };

        // Allow TunnelExit to be reached from multiple exec paths (multi-exit macros).
        // Each path terminates (CurrentExecPin = nullptr), so no infinite loop risk.
        if (State.VisitedNodes.Contains(Node) && Semantics == ENodeSemantics::TunnelExit)
        {
            // Fall through to TunnelExit handler — don't trigger reconvergence
        }
        // Reconvergence detection: node already visited
        else if (State.VisitedNodes.Contains(Node))
        {
            // Find which label this node was originally emitted under
            FString* OrigLabel = State.NodeToEmittedLabel.Find(Node);
            if (OrigLabel && !OrigLabel->IsEmpty())
            {
                // Node was emitted inside a labeled block — reference that label
                State.Lines.Add(FormatExecGotoLine(*OrigLabel, CurrentExecPin));
            }
            else
            {
                // Node was in the initial (pre-label) block.
                // We need to retroactively insert a label before it.
                FString NewLabel = State.AllocLabel(TEXT("merge"));

                // Direct index lookup instead of fragile string search
                if (const int32* LineIdxPtr = State.NodeToLineIndex.Find(Node))
                {
                    // Copy before mutating the map — LineIdxPtr points into NodeToLineIndex
                    const int32 LineIdx = *LineIdxPtr;

                    // Insert blank line + label before the node's emission line
                    State.Lines.Insert(FString(), LineIdx);
                    State.Lines.Insert(FString::Printf(TEXT("@%s:"), *NewLabel), LineIdx + 1);

                    // Adjust all line indices after the insertion point
                    // (two lines were inserted, so bump by 2)
                    for (auto& Pair : State.NodeToLineIndex)
                    {
                        if (Pair.Value >= LineIdx)
                        {
                            Pair.Value += 2;
                        }
                    }
                    State.NodeToEmittedLabel.Add(Node, NewLabel);
                    State.Lines.Add(FormatExecGotoLine(NewLabel, CurrentExecPin));
                }
                else
                {
                    State.Warnings.Add(FBpirWarning{FString::Printf(
                        TEXT("Reconvergence: could not find line index for node '%s'"),
                        *Node->GetNodeTitle(ENodeTitleType::MenuTitle).ToString()), EBpirWarningSeverity::Error});
                    State.Lines.Add(TEXT("    # [WARNING] reconvergence target not resolvable"));
                }
            }
            break;
        }

        // Same tunnel-exclusion carve-out as MakeExecTarget: macro instances are
        // valid shared-exec targets even though UK2Node_MacroInstance : UK2Node_Tunnel.
        const bool bNodeIsPureTunnel = Node->IsA<UK2Node_Tunnel>() && !Node->IsA<UK2Node_MacroInstance>();
        if (!bNodeIsPureTunnel && IsSharedExecInputPin(LinkedPin))
        {
            if (FString* MergeLabel = State.NodeToEmittedLabel.Find(Node))
            {
                if (!MergeLabel->IsEmpty() && *MergeLabel != State.CurrentWalkLabel)
                {
                    State.Lines.Add(FormatExecGotoLine(*MergeLabel, CurrentExecPin));
                    break;
                }
            }
            else
            {
                const FString NewMergeLabel = State.AllocLabel(TEXT("merge"));
                State.NodeToEmittedLabel.Add(Node, NewMergeLabel);
                State.PendingLabels.Add({NewMergeLabel, CurrentExecPin});
                State.Lines.Add(FormatExecGotoLine(NewMergeLabel, CurrentExecPin));
                break;
            }
        }

        State.VisitedNodes.Add(Node);
        State.NodeToEmittedLabel.Add(Node, State.CurrentWalkLabel);
        bEmittedExecutableNode = true;

        // Create a resolve lambda that resolves pin values using our state
        auto ResolvePin = [this, &State](UEdGraphPin* Pin) -> FString
        {
            return ResolveInputValue(Pin, State);
        };

        // ------------------------------------------------------------------
        // Branch
        // ------------------------------------------------------------------
        if (Semantics == ENodeSemantics::Branch)
        {
            EmitPureDependencies(Node, State);
            FString ValueName = State.AllocValueName();
            State.NodeToValueName.Add(Node, ValueName);

            // Find exec output pins first to determine which branches are connected
            UEdGraphPin* TruePin = nullptr;
            UEdGraphPin* FalsePin = nullptr;
            for (UEdGraphPin* Pin : Node->Pins)
            {
                if (Pin && Pin->Direction == EGPD_Output
                    && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
                {
                    if (Pin->PinName == UEdGraphSchema_K2::PN_Then)
                    {
                        TruePin = Pin;
                    }
                    else if (Pin->PinName == UEdGraphSchema_K2::PN_Else)
                    {
                        FalsePin = Pin;
                    }
                }
            }

            // Only allocate labels and emit exec targets for connected branches
            FBpirLabelMap LabelMap;
            if (TruePin && TruePin->LinkedTo.Num() > 0)
            {
                LabelMap.Add(TEXT("true"), MakeExecTarget(TEXT("then"), TruePin));
            }
            if (FalsePin && FalsePin->LinkedTo.Num() > 0)
            {
                LabelMap.Add(TEXT("false"), MakeExecTarget(TEXT("else"), FalsePin));
            }

            FString Line = TextEmitter->EmitBranch(Node, ValueName, LabelMap, ResolvePin);
            AppendNodeLine(State, Node, Line);

            CurrentExecPin = nullptr;
            continue;
        }

        // ------------------------------------------------------------------
        // Switch
        // ------------------------------------------------------------------
        if (Semantics == ENodeSemantics::Switch)
        {
            EmitPureDependencies(Node, State);
            FString ValueName = State.AllocValueName();
            State.NodeToValueName.Add(Node, ValueName);

            FSwitchInfo SwitchInfo = GraphWalker->AnalyzeSwitch(Node);
            FBpirLabelMap LabelMap;

            for (const auto& Case : SwitchInfo.Cases)
            {
                if (Case.Value && Case.Value->LinkedTo.Num() > 0)
                {
                    FString CaseLabel = State.AllocLabel(FString::Printf(TEXT("case_%s"), *Case.Key));
                    LabelMap.Add(Case.Key, FBpirEmitTarget{ CaseLabel, GetTargetInputPinNameIfMultiInput(Case.Value) });
                    State.PendingLabels.Add({CaseLabel, Case.Value});
                }
            }

            if (SwitchInfo.DefaultPin && SwitchInfo.DefaultPin->LinkedTo.Num() > 0)
            {
                FString DefaultLabel = State.AllocLabel(TEXT("default"));
                LabelMap.Add(TEXT("default"), FBpirEmitTarget{ DefaultLabel, GetTargetInputPinNameIfMultiInput(SwitchInfo.DefaultPin) });
                State.PendingLabels.Add({DefaultLabel, SwitchInfo.DefaultPin});
            }

            FString Line = TextEmitter->EmitSwitch(Node, ValueName, LabelMap, ResolvePin);
            AppendNodeLine(State, Node, Line);

            CurrentExecPin = nullptr;
            continue;
        }

        // ------------------------------------------------------------------
        // Sequence
        // ------------------------------------------------------------------
        if (Semantics == ENodeSemantics::Sequence)
        {
            FString ValueName = State.AllocValueName();
            State.NodeToValueName.Add(Node, ValueName);

            TArray<UEdGraphPin*> SeqPins = GraphWalker->GetAllExecOutputPins(Node);
            FBpirLabelMap LabelMap;

            for (int32 i = 0; i < SeqPins.Num(); ++i)
            {
                if (SeqPins[i] && SeqPins[i]->LinkedTo.Num() > 0)
                {
                    FString PinLabel = State.AllocLabel(FString::Printf(TEXT("s%d"), i));
                    FString PinKey = FString::Printf(TEXT("%d"), i);
                    LabelMap.Add(PinKey, FBpirEmitTarget{ PinLabel, GetTargetInputPinNameIfMultiInput(SeqPins[i]) });
                    State.PendingLabels.Add({PinLabel, SeqPins[i]});
                }
            }

            FString Line = TextEmitter->EmitSequence(Node, ValueName, LabelMap);
            AppendNodeLine(State, Node, Line);

            CurrentExecPin = nullptr;
            continue;
        }

        // ------------------------------------------------------------------
        // ForEach / WhileLoop
        // ------------------------------------------------------------------
        if (Semantics == ENodeSemantics::ForEach || Semantics == ENodeSemantics::WhileLoop)
        {
            EmitPureDependencies(Node, State);
            FString ValueName = State.AllocValueName();
            State.NodeToValueName.Add(Node, ValueName);

            // Find exec output pins first to determine which paths are connected
            UEdGraphPin* BodyPin = Node->FindPin(TEXT("Loop Body"), EGPD_Output);
            if (!BodyPin)
            {
                BodyPin = Node->FindPin(TEXT("LoopBody"), EGPD_Output);
            }
            if (!BodyPin)
            {
                BodyPin = GraphWalker->GetExecOutputPin(Node, 0);
            }
            UEdGraphPin* CompletedPin = GraphWalker->GetCompletionPin(Node);

            FBpirLabelMap LabelMap;
            if (BodyPin && BodyPin->LinkedTo.Num() > 0)
            {
                FString BodyLabel = State.AllocLabel(TEXT("body"));
                LabelMap.Add(TEXT("body"), FBpirEmitTarget{ BodyLabel, GetTargetInputPinNameIfMultiInput(BodyPin) });
                State.PendingLabels.Add({BodyLabel, BodyPin});
            }
            if (CompletedPin && CompletedPin->LinkedTo.Num() > 0)
            {
                FString DoneLabel = State.AllocLabel(TEXT("done"));
                LabelMap.Add(TEXT("completed"), FBpirEmitTarget{ DoneLabel, GetTargetInputPinNameIfMultiInput(CompletedPin) });
                State.PendingLabels.Add({DoneLabel, CompletedPin});
            }

            FString Line = TextEmitter->EmitLoop(Node, ValueName, LabelMap, ResolvePin);
            AppendNodeLine(State, Node, Line);

            CurrentExecPin = nullptr;
            continue;
        }

        // ------------------------------------------------------------------
        // Cast
        // ------------------------------------------------------------------
        if (Semantics == ENodeSemantics::Cast)
        {
            EmitPureDependencies(Node, State);
            FString ValueName = State.AllocValueName();
            State.NodeToValueName.Add(Node, ValueName);

            // Find exec output pins first to determine which paths are connected
            UEdGraphPin* SuccessPin = GraphWalker->GetExecOutputPin(Node, 0);
            UEdGraphPin* FailPin = GraphWalker->GetExecOutputPin(Node, 1);

            FBpirLabelMap LabelMap;
            if (SuccessPin && SuccessPin->LinkedTo.Num() > 0)
            {
                FString OkLabel = State.AllocLabel(TEXT("ok"));
                LabelMap.Add(TEXT("success"), FBpirEmitTarget{ OkLabel, GetTargetInputPinNameIfMultiInput(SuccessPin) });
                State.PendingLabels.Add({OkLabel, SuccessPin});
            }
            if (FailPin && FailPin->LinkedTo.Num() > 0)
            {
                FString FailLabel = State.AllocLabel(TEXT("fail"));
                LabelMap.Add(TEXT("fail"), FBpirEmitTarget{ FailLabel, GetTargetInputPinNameIfMultiInput(FailPin) });
                State.PendingLabels.Add({FailLabel, FailPin});
            }

            if (UK2Node_DynamicCast* CastNode = Cast<UK2Node_DynamicCast>(Node))
            {
                if (CastNode->TargetType == nullptr)
                {
                    State.Warnings.Add(FBpirWarning{FString::Printf(
                        TEXT("Cast node has no resolved target type (emitted as cast<Unknown>): asset=%s graph=%s class=%s '%s' nodeId=%s"),
                        TargetBlueprint ? *TargetBlueprint->GetPathName() : TEXT("<null>"),
                        *CurrentSourceGraphName,
                        *Node->GetClass()->GetName(),
                        *Node->GetNodeTitle(ENodeTitleType::ListView).ToString(),
                        *Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens))});
                }
            }

            FString Line = TextEmitter->EmitCast(Node, ValueName, LabelMap, ResolvePin);
            AppendNodeLine(State, Node, Line);

            CurrentExecPin = nullptr;
            continue;
        }

        // ------------------------------------------------------------------
        // Latent
        // ------------------------------------------------------------------
        if (Semantics == ENodeSemantics::Latent)
        {
            EmitPureDependencies(Node, State);
            FString ValueName = State.AllocValueName();
            State.NodeToValueName.Add(Node, ValueName);

            FBpirLabelMap LabelMap;

            // Collect all connected exec output pins for latent nodes
            TArray<UEdGraphPin*> ExecOuts = GraphWalker->GetAllExecOutputPins(Node);
            for (UEdGraphPin* ExecOut : ExecOuts)
            {
                if (!ExecOut || ExecOut->LinkedTo.Num() == 0) continue;
                FString PinName = ExecOut->PinName.ToString();
                PinName.ReplaceInline(TEXT(" "), TEXT(""));

                // Use "completed" as the canonical name for the completion pin
                FString LabelName;
                if (PinName == TEXT("Completed") || PinName == TEXT("OnFinished") || PinName == TEXT("Then"))
                {
                    LabelName = State.AllocLabel(TEXT("after"));
                    LabelMap.Add(TEXT("completed"), FBpirEmitTarget{ LabelName, GetTargetInputPinNameIfMultiInput(ExecOut) });
                }
                else
                {
                    LabelName = State.AllocLabel(PinName.ToLower());
                    LabelMap.Add(PinName, FBpirEmitTarget{ LabelName, GetTargetInputPinNameIfMultiInput(ExecOut) });
                }

                State.PendingLabels.Add({LabelName, ExecOut});
            }

            // If no exec targets were found, add a default completed target
            if (LabelMap.Num() == 0)
            {
                UEdGraphPin* CompPin = GraphWalker->GetCompletionPin(Node);
                if (CompPin && CompPin->LinkedTo.Num() > 0)
                {
                    FString CompletedLabel = State.AllocLabel(TEXT("after"));
                    LabelMap.Add(TEXT("completed"), FBpirEmitTarget{ CompletedLabel, GetTargetInputPinNameIfMultiInput(CompPin) });
                    State.PendingLabels.Add({CompletedLabel, CompPin});
                }
            }

            FString Line = TextEmitter->EmitLatent(Node, ValueName, LabelMap, ResolvePin);
            AppendNodeLine(State, Node, Line);

            CurrentExecPin = nullptr;
            continue;
        }

        // ------------------------------------------------------------------
        // Timeline
        // ------------------------------------------------------------------
        if (Semantics == ENodeSemantics::Timeline)
        {
            FString ValueName = State.AllocValueName();
            State.NodeToValueName.Add(Node, ValueName);

            TArray<UEdGraphPin*> ExecOuts = GraphWalker->GetAllExecOutputPins(Node);
            FBpirLabelMap LabelMap;

            for (UEdGraphPin* ExecOut : ExecOuts)
            {
                if (!ExecOut || ExecOut->LinkedTo.Num() == 0) continue;
                FString PinName = ExecOut->PinName.ToString();
                PinName.ReplaceInline(TEXT(" "), TEXT(""));
                FString LabelName = State.AllocLabel(PinName.ToLower());
                LabelMap.Add(PinName.ToLower(), FBpirEmitTarget{ LabelName, GetTargetInputPinNameIfMultiInput(ExecOut) });
                State.PendingLabels.Add({LabelName, ExecOut});
            }

            FString Line = TextEmitter->EmitTimeline(Node, ValueName, LabelMap);
            AppendNodeLine(State, Node, Line);

            CurrentExecPin = nullptr;
            continue;
        }

        // ------------------------------------------------------------------
        // DoOnce, Gate, FlipFlop, MacroInstance (other macros)
        // ------------------------------------------------------------------
        if (Semantics == ENodeSemantics::DoOnce
            || Semantics == ENodeSemantics::Gate
            || Semantics == ENodeSemantics::FlipFlop
            || Semantics == ENodeSemantics::MacroInstance)
        {
            EmitPureDependencies(Node, State);
            FString ValueName = State.AllocValueName();
            State.NodeToValueName.Add(Node, ValueName);

            TArray<UEdGraphPin*> ExecOuts = GraphWalker->GetAllExecOutputPins(Node);
            FBpirLabelMap LabelMap;

            // Build label map from connected exec output pins only
            for (int32 i = 0; i < ExecOuts.Num(); ++i)
            {
                UEdGraphPin* ExecOut = ExecOuts[i];
                if (!ExecOut || ExecOut->LinkedTo.Num() == 0) continue;

                FString PinName = ExecOut->PinName.ToString();
                PinName.ReplaceInline(TEXT(" "), TEXT(""));

                const FString TargetInputPinName = GetTargetInputPinNameIfMultiInput(ExecOut);

                FString LabelName;
                if (Semantics == ENodeSemantics::DoOnce && PinName == TEXT("Completed"))
                {
                    LabelName = State.AllocLabel(TEXT("go"));
                    LabelMap.Add(TEXT("completed"), FBpirEmitTarget{ LabelName, TargetInputPinName });
                }
                else if (Semantics == ENodeSemantics::FlipFlop)
                {
                    LabelName = State.AllocLabel(FString::Printf(TEXT("path%s"), *PinName));
                    LabelMap.Add(PinName, FBpirEmitTarget{ LabelName, TargetInputPinName });
                }
                else if (Semantics == ENodeSemantics::Gate && PinName == TEXT("Exit"))
                {
                    LabelName = State.AllocLabel(TEXT("through"));
                    LabelMap.Add(TEXT("exit"), FBpirEmitTarget{ LabelName, TargetInputPinName });
                }
                else
                {
                    LabelName = State.AllocLabel(PinName.ToLower());
                    LabelMap.Add(PinName, FBpirEmitTarget{ LabelName, TargetInputPinName });
                }

                State.PendingLabels.Add({LabelName, ExecOut});
            }

            FString Line = TextEmitter->EmitMacro(Node, ValueName, LabelMap, ResolvePin);
            AppendNodeLine(State, Node, Line);

            CurrentExecPin = nullptr;
            continue;
        }

        // ------------------------------------------------------------------
        // Variable set
        // ------------------------------------------------------------------
        if (Semantics == ENodeSemantics::VariableSet)
        {
            EmitPureDependencies(Node, State);
            // Variable sets don't produce a named value
            State.NodeToValueName.Add(Node, FString());

            FString Line = TextEmitter->EmitVariableSet(Node, ResolvePin);
            AppendNodeLine(State, Node, Line);

            CurrentExecPin = GraphWalker->GetExecOutputPin(Node, 0);
            continue;
        }

        // ------------------------------------------------------------------
        // Return
        // ------------------------------------------------------------------
        if (Semantics == ENodeSemantics::Return)
        {
            EmitPureDependencies(Node, State);

            FString Line = TextEmitter->EmitReturn(Node, ResolvePin);
            AppendNodeLine(State, Node, Line);

            CurrentExecPin = nullptr;
            continue;
        }

        // ------------------------------------------------------------------
        // TunnelExit — exit tunnel in macro graph (analogous to Return)
        // ------------------------------------------------------------------
        if (Semantics == ENodeSemantics::TunnelExit)
        {
            EmitPureDependencies(Node, State);

            // Determine exit pin name for multi-exit macros
            FString ExitPinName;
            bool bMultiExit = false;
            int32 ExecFound = 0;
            for (UEdGraphPin* Pin : Node->Pins)
            {
                if (Pin && Pin->Direction == EGPD_Input
                    && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
                {
                    if (++ExecFound > 1) { bMultiExit = true; break; }
                }
            }
            if (bMultiExit && LinkedPin)
            {
                ExitPinName = LinkedPin->PinName.ToString();
            }

            FString Line = TextEmitter->EmitMacroReturn(Node, ResolvePin, ExitPinName);
            AppendNodeLine(State, Node, Line);

            CurrentExecPin = nullptr;
            continue;
        }

        // ------------------------------------------------------------------
        // Dispatcher (event dispatcher call/bind/unbind/clear)
        // ------------------------------------------------------------------
        if (Semantics == ENodeSemantics::Dispatcher)
        {
            EmitPureDependencies(Node, State);

            // Check if this node's output data pins are consumed by downstream nodes
            bool bHasUsedOutput = false;
            for (UEdGraphPin* Pin : Node->Pins)
            {
                if (Pin && Pin->Direction == EGPD_Output
                    && Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec
                    && Pin->LinkedTo.Num() > 0)
                {
                    bHasUsedOutput = true;
                    break;
                }
            }

            FString ValueName;
            if (bHasUsedOutput)
            {
                ValueName = State.AllocValueName();
                State.NodeToValueName.Add(Node, ValueName);
            }
            else
            {
                State.NodeToValueName.Add(Node, FString());
            }

            FString Line = TextEmitter->EmitDispatcherNode(Node, ValueName, ResolvePin);
            AppendNodeLine(State, Node, Line);

            CurrentExecPin = GraphWalker->GetExecOutputPin(Node, 0);
            continue;
        }

        // ------------------------------------------------------------------
        // Plain function call (single exec output)
        // ------------------------------------------------------------------
        if (Semantics == ENodeSemantics::FunctionCall)
        {
            EmitPureDependencies(Node, State);

            // Check if this node's output data pins are consumed by downstream nodes
            bool bHasUsedOutput = false;
            for (UEdGraphPin* Pin : Node->Pins)
            {
                if (Pin && Pin->Direction == EGPD_Output
                    && Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec
                    && Pin->LinkedTo.Num() > 0)
                {
                    bHasUsedOutput = true;
                    break;
                }
            }

            FString ValueName;
            if (bHasUsedOutput)
            {
                ValueName = State.AllocValueName();
                State.NodeToValueName.Add(Node, ValueName);
            }
            else
            {
                State.NodeToValueName.Add(Node, FString());
            }

            FString Line = TextEmitter->EmitCallNode(Node, ValueName, ResolvePin);
            AppendNodeLine(State, Node, Line);

            CurrentExecPin = GraphWalker->GetExecOutputPin(Node, 0);
            continue;
        }

        // ------------------------------------------------------------------
        // Validated VariableGet (exec-input variant)
        // Pure VariableGets never reach here; they are inlined by ResolveInputValue.
        // The Validated Object variant (SetPurity(false)) carries an Execute
        // input plus two exec outputs: PN_Then (valid) and PN_Else (invalid).
        // Branch variants on other types may carry only a single Then output,
        // so accept any node with at least one exec output.
        // ------------------------------------------------------------------
        if (Semantics == ENodeSemantics::VariableGet)
        {
            TArray<UEdGraphPin*> ExecOuts = GraphWalker->GetAllExecOutputPins(Node);

            if (ExecOuts.Num() >= 1)
            {
                EmitPureDependencies(Node, State);

                // Allocate a value name only when the data output pin is consumed.
                bool bHasUsedOutput = false;
                for (UEdGraphPin* Pin : Node->Pins)
                {
                    if (Pin && Pin->Direction == EGPD_Output
                        && Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec
                        && Pin->LinkedTo.Num() > 0)
                    {
                        bHasUsedOutput = true;
                        break;
                    }
                }

                FString ValueName;
                if (bHasUsedOutput)
                {
                    ValueName = State.AllocValueName();
                    State.NodeToValueName.Add(Node, ValueName);
                }
                else
                {
                    State.NodeToValueName.Add(Node, FString());
                }

                // Build the LabelMap from every connected exec output, keyed by
                // the original pin name lowercased so each pin (then/else/...)
                // gets its own block. Disconnected pins are skipped — BPIR
                // recompile rejects label references that have no body.
                FBpirLabelMap LabelMap;
                for (UEdGraphPin* ExecOut : ExecOuts)
                {
                    if (!ExecOut || ExecOut->LinkedTo.Num() == 0) continue;

                    FString PinName = ExecOut->PinName.ToString();
                    PinName.ReplaceInline(TEXT(" "), TEXT(""));
                    const FString PinKey = PinName.ToLower();

                    FString LabelName = State.AllocLabel(PinKey);
                    LabelMap.Add(PinKey, FBpirEmitTarget{ LabelName, GetTargetInputPinNameIfMultiInput(ExecOut) });
                    State.PendingLabels.Add({LabelName, ExecOut});
                }

                FString Line = TextEmitter->EmitVariableGet(Node, ValueName, LabelMap);
                AppendNodeLine(State, Node, Line);

                // Multiple exec outputs branch into pending blocks; no straight
                // exec successor to advance to.
                CurrentExecPin = (ExecOuts.Num() == 1)
                    ? GraphWalker->GetExecOutputPin(Node, 0)
                    : nullptr;
                continue;
            }
        }

        // ------------------------------------------------------------------
        // Fallthrough: unknown node — use smart generic fallback
        // ------------------------------------------------------------------
        {
            EmitPureDependencies(Node, State);

            // Determine exec output structure
            TArray<UEdGraphPin*> ExecOuts = GraphWalker->GetAllExecOutputPins(Node);
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 6, 0)
            // GetFactoryFunction() was made public in UE 5.6; on 5.4/5.5 it is protected
            const bool bIsConfiguredAsyncAction =
                Node->IsA<UK2Node_AsyncAction>()
                && Cast<UK2Node_AsyncAction>(Node)->GetFactoryFunction() != nullptr;
#else
            // On UE 5.4/5.5 GetFactoryFunction() is protected; use UObject reflection to read
            // ProxyFactoryFunctionName (also protected UPROPERTY) without subclassing.
            // A non-None name means the node was initialized with a factory function.
            bool bIsConfiguredAsyncAction = false;
            if (UK2Node_AsyncAction* AsyncNode = Cast<UK2Node_AsyncAction>(Node))
            {
                if (const FNameProperty* NameProp = FindFProperty<FNameProperty>(
                        UK2Node_BaseAsyncTask::StaticClass(), TEXT("ProxyFactoryFunctionName")))
                {
                    const FName FactoryName = NameProp->GetPropertyValue_InContainer(AsyncNode);
                    bIsConfiguredAsyncAction = !FactoryName.IsNone();
                }
            }
#endif

            auto SelectedEmit = [&](const FString& ValueName, const FBpirLabelMap& Labels) -> FString
            {
                if (bIsConfiguredAsyncAction)
                {
                    return TextEmitter->EmitAsyncActionNode(Node, ValueName, Labels, ResolvePin);
                }
                return TextEmitter->EmitGenericNode(Node, ValueName, Labels, ResolvePin);
            };

            // Check if this node has any exec pins at all (input or output)
            bool bHasAnyExecPin = false;
            for (UEdGraphPin* Pin : Node->Pins)
            {
                if (Pin && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
                {
                    bHasAnyExecPin = true;
                    break;
                }
            }

            if (!bHasAnyExecPin)
            {
                // Pure node with no exec pins — use EmitPureNode as safety net
                FString ValueName = State.AllocValueName();
                State.NodeToValueName.Add(Node, ValueName);

                FString Line = TextEmitter->EmitPureNode(Node, ValueName, ResolvePin);
                AppendNodeLine(State, Node, Line);

                CurrentExecPin = nullptr;
            }
            else if (ExecOuts.Num() <= 1)
            {
                // Single exec output — emit as generic call, continue chain
                bool bHasUsedOutput = false;
                for (UEdGraphPin* Pin : Node->Pins)
                {
                    if (Pin && Pin->Direction == EGPD_Output
                        && Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec
                        && Pin->LinkedTo.Num() > 0)
                    {
                        bHasUsedOutput = true;
                        break;
                    }
                }

                FString ValueName;
                if (bHasUsedOutput)
                {
                    ValueName = State.AllocValueName();
                    State.NodeToValueName.Add(Node, ValueName);
                }
                else
                {
                    State.NodeToValueName.Add(Node, FString());
                }

                FBpirLabelMap EmptyLabelMap;
                FString Line = SelectedEmit(ValueName, EmptyLabelMap);
                AppendNodeLine(State, Node, Line);

                CurrentExecPin = GraphWalker->GetExecOutputPin(Node, 0);
            }
            else
            {
                // Multiple exec outputs — build LabelMap and queue pending labels
                FString ValueName = State.AllocValueName();
                State.NodeToValueName.Add(Node, ValueName);

                FBpirLabelMap LabelMap;
                for (UEdGraphPin* ExecOut : ExecOuts)
                {
                    // Skip disconnected output pins: BPIR recompile rejects label
                    // references that have no corresponding block body, so emitting
                    // labels only for pins we actually write bodies for keeps the
                    // decompile output round-trippable.
                    if (!ExecOut || ExecOut->LinkedTo.Num() == 0) continue;
                    FString PinName = ExecOut->PinName.ToString();
                    PinName.ReplaceInline(TEXT(" "), TEXT(""));
                    FString LabelName = State.AllocLabel(PinName.ToLower());
                    LabelMap.Add(PinName, FBpirEmitTarget{ LabelName, GetTargetInputPinNameIfMultiInput(ExecOut) });
                    State.PendingLabels.Add({LabelName, ExecOut});
                }

                FString Line = SelectedEmit(ValueName, LabelMap);
                AppendNodeLine(State, Node, Line);

                CurrentExecPin = nullptr;
            }
        }
    }

    // A straight-line node with a real but disconnected default exec output has no
    // successor. Serialize that fact so a later queued label cannot be interpreted
    // as the documented cross-label fall-through target.
    if (bEmittedExecutableNode && CurrentExecPin && CurrentExecPin->LinkedTo.Num() == 0)
    {
        State.Lines.Add(FString::Printf(
            TEXT("    %s"),
            BpirSharedConstants::Keywords::End));
    }
}

// ---------------------------------------------------------------------------
// Type-default literal helper
// ---------------------------------------------------------------------------

TArray<FString> FBpirDecompiler::ExtractStructComponentValues(const FString& StructLiteral)
{
    // Flat split on `,` then take the substring after `=` from each chunk.
    // Nested struct values containing literal commas would be mis-split, but
    // ExportText for the sugared-positional types (FVector/FRotator/FLinearColor)
    // emits only scalar component values at this level — no nested commas.
    TArray<FString> Values;
    if (StructLiteral.StartsWith(TEXT("(")) && StructLiteral.EndsWith(TEXT(")")))
    {
        FString Inner = StructLiteral.Mid(1, StructLiteral.Len() - 2);
        TArray<FString> Parts;
        Inner.ParseIntoArray(Parts, TEXT(","));
        for (const FString& Part : Parts)
        {
            int32 EqIdx = INDEX_NONE;
            if (Part.FindChar(TEXT('='), EqIdx))
            {
                Values.Add(Part.Mid(EqIdx + 1).TrimStartAndEnd());
            }
        }
    }
    return Values;
}

FString FBpirDecompiler::FormatPinDefaultLiteral(const UEdGraphPin* Pin)
{
    if (!Pin)
    {
        return TEXT("<unresolved>");
    }

    const FName& Category = Pin->PinType.PinCategory;

    if (Category == UEdGraphSchema_K2::PC_Object
        || Category == UEdGraphSchema_K2::PC_Class
        || Category == UEdGraphSchema_K2::PC_Interface
        || Category == UEdGraphSchema_K2::PC_SoftObject
        || Category == UEdGraphSchema_K2::PC_SoftClass)
    {
        return TEXT("nullptr");
    }

    if (Category == UEdGraphSchema_K2::PC_String
        || Category == UEdGraphSchema_K2::PC_Text
        || Category == UEdGraphSchema_K2::PC_FieldPath)
    {
        return TEXT("\"\"");
    }

    if (Category == UEdGraphSchema_K2::PC_Boolean)
    {
        return TEXT("false");
    }

    if (Category == UEdGraphSchema_K2::PC_Byte
        || Category == UEdGraphSchema_K2::PC_Int
        || Category == UEdGraphSchema_K2::PC_Int64)
    {
        return TEXT("0");
    }

    if (Category == UEdGraphSchema_K2::PC_Float
        || Category == UEdGraphSchema_K2::PC_Double
        || Category == UEdGraphSchema_K2::PC_Real)
    {
        return TEXT("0.0");
    }

    if (Category == UEdGraphSchema_K2::PC_Name)
    {
        return TEXT("None");
    }

    if (Category == UEdGraphSchema_K2::PC_Struct)
    {
        if (UScriptStruct* StructType = Cast<UScriptStruct>(Pin->PinType.PinSubCategoryObject.Get()))
        {
            // Default-construct then ExportText to a (Key=Val,...) literal, then
            // route through the same Case-2 sugaring used for non-empty struct
            // defaults so FVector/FRotator/FLinearColor render in their compact
            // form and other structs pass through as parenthesized text.
            FStructOnScope DefaultInstance(StructType);
            FString Exported;
            StructType->ExportText(Exported, DefaultInstance.GetStructMemory(),
                DefaultInstance.GetStructMemory(), nullptr, PPF_None, nullptr);

            if (StructType == TBaseStructure<FLinearColor>::Get()
                || StructType == TBaseStructure<FVector>::Get()
                || StructType == TBaseStructure<FRotator>::Get())
            {
                TArray<FString> Values = ExtractStructComponentValues(Exported);
                const FString Sugared = BpirStructLiteralUtils::FormatStructComponentsAsBpir(StructType->GetName(), Values);
                if (!Sugared.IsEmpty())
                {
                    return Sugared;
                }
            }

            if (!Exported.IsEmpty())
            {
                return Exported;
            }
        }
    }

    return TEXT("<unresolved>");
}

// ---------------------------------------------------------------------------
// Value reference resolution
// ---------------------------------------------------------------------------

FString FBpirDecompiler::ResolveInputValue(UEdGraphPin* InputPin, FEntryState& State, int32 Depth)
{
    if (!InputPin)
    {
        return TEXT("?");
    }
    if (Depth > 64)
    {
        // Circular pin reference — treat as implicit self
        return TEXT("self");
    }

    // Case 1: pin is connected to another node's output
    if (InputPin->LinkedTo.Num() > 0)
    {
        if (InputPin->LinkedTo.Num() > 1)
        {
            // Multi-self fan-out emits one statement per LinkedTo entry, so the
            // "only the first is used" warning is a false positive there.
            const bool bMultiSelfFanOut =
                InputPin->PinName == UEdGraphSchema_K2::PN_Self
                && BpirDecompiler::Helpers::NodeSupportsMultiSelf(InputPin->GetOwningNode());
            if (!bMultiSelfFanOut)
            {
                State.Warnings.Add(FBpirWarning{FString::Printf(
                    TEXT("Data pin '%s' has %d connections but only the first is used"),
                    *InputPin->PinName.ToString(), InputPin->LinkedTo.Num())});
            }
        }

        UEdGraphPin* SourcePin = InputPin->LinkedTo[0];

        // Follow through reroute (knot) nodes to the real source. Dead-end knots
        // with a default on their KnotInput recurse via ResolveInputValue.
        UEdGraphPin* DeadEndKnotInput = nullptr;
        SourcePin = BpirDecompiler::Helpers::FollowKnotsBackward(SourcePin, DeadEndKnotInput);
        if (!SourcePin && DeadEndKnotInput)
        {
            return ResolveInputValue(DeadEndKnotInput, State, Depth + 1);
        }
        UEdGraphNode* SourceNode = SourcePin ? SourcePin->GetOwningNode() : nullptr;

        if (!SourceNode)
        {
            State.Warnings.Add(FBpirWarning{FString::Printf(
                TEXT("Unresolvable value: pin '%s' connected through knot to null source"),
                *InputPin->PinName.ToString())});
            return FormatPinDefaultLiteral(InputPin);
        }

        // Self node -> "self"
        if (SourceNode->IsA<UK2Node_Self>())
        {
            return TEXT("self");
        }

        // Entry node parameter pins -> $ParamName
        if (SourceNode->IsA<UK2Node_CustomEvent>()
            || SourceNode->IsA<UK2Node_Event>()
            || SourceNode->IsA<UK2Node_FunctionEntry>()
            || FCodeNodeEmitter::GetEnhancedInputAction(SourceNode))
        {
            const FString Formatted = TryFormatEntryParam(SourcePin);
            if (!Formatted.IsEmpty())
            {
                return Formatted;
            }
        }

        // Macro-entry tunnel parameter pins -> $ParamName
        // The macro-entry tunnel has output data pins exposed to the body (matches
        // the predicate in GraphWalker.cpp that classifies it as TunnelEntry).
        // UK2Node_MacroInstance also derives from UK2Node_Tunnel, but instances are
        // routed via the NodeToValueName lookup below before reaching this branch.
        if (BlueprintHandlerUtils::IsMacroEntryTunnel(SourceNode))
        {
            const FString Formatted = TryFormatEntryParam(SourcePin);
            if (!Formatted.IsEmpty())
            {
                return Formatted;
            }
        }

        // UK2Node_InputKey exposes a constant FKey via its `Key` output pin.
        // The node is treated as an entry node by the emitter, so it never
        // appears in NodeToValueName — without this branch the resolver falls
        // through to the "visited but produced no value name" warning and
        // emits `?`. Render the bound key as an inline literal that matches
        // the entry-signature form (whitespace stripped, e.g. `SpaceBar`).
        if (UK2Node_InputKey* InputKeyNode = Cast<UK2Node_InputKey>(SourceNode))
        {
            if (SourcePin && SourcePin->PinName == TEXT("Key"))
            {
                FString KeyName = FBpirInputKeyHelpers::FormatInputKeyAsBpirIdentifier(InputKeyNode->InputKey);
                if (!KeyName.IsEmpty())
                {
                    return KeyName;
                }
            }
        }

        // Variable get -> "$VarName" or "$target.VarName" (external). A split struct
        // output pin contributes its member path, so a read of one member never
        // renders as the bare variable (which would also drop the member entirely).
        if (UK2Node_VariableGet* GetNode = Cast<UK2Node_VariableGet>(SourceNode))
        {
            FString VarName = FBpirTextEmitter::GetVariableName(SourceNode);
            const FString MemberSuffix = FormatSplitMemberSuffix(SourcePin);
            if (!GetNode->VariableReference.IsSelfContext())
            {
                UEdGraphPin* SelfPin = GetNode->FindPin(UEdGraphSchema_K2::PN_Self);
                if (SelfPin && SelfPin->LinkedTo.Num() > 0)
                {
                    FString TargetRef = ResolveInputValue(SelfPin, State, Depth + 1);
                    return FString::Printf(TEXT("%s.%s%s"), *TargetRef, *VarName, *MemberSuffix);
                }
            }
            return FString::Printf(TEXT("$%s%s"), *VarName, *MemberSuffix);
        }

        // Variable set Output_Get -> "$VarName" or "$target.VarName" (external)
        if (UK2Node_VariableSet* SetNode = Cast<UK2Node_VariableSet>(SourceNode))
        {
            FString VarName = FBpirTextEmitter::GetVariableName(SourceNode);
            const FString MemberSuffix = FormatSplitMemberSuffix(SourcePin);
            if (!SetNode->VariableReference.IsSelfContext())
            {
                UEdGraphPin* SelfPin = SetNode->FindPin(UEdGraphSchema_K2::PN_Self);
                if (SelfPin && SelfPin->LinkedTo.Num() > 0)
                {
                    FString TargetRef = ResolveInputValue(SelfPin, State, Depth + 1);
                    return FString::Printf(TEXT("%s.%s%s"), *TargetRef, *VarName, *MemberSuffix);
                }
            }
            return FString::Printf(TEXT("$%s%s"), *VarName, *MemberSuffix);
        }

        // Check if source node already has a value name
        FString* ExistingName = State.NodeToValueName.Find(SourceNode);
        if (ExistingName && !ExistingName->IsEmpty())
        {
            // If the output pin is not "ReturnValue", use %name.PinName. A split
            // struct sub-pin is addressed through its root pin plus a dotted member
            // suffix (%n0.OutHit.BoneName, or %n0.BoneName when the root collapses
            // to the bare register), so sibling members never share one spelling.
            UEdGraphPin* const RootPin = GetSplitPinRoot(SourcePin);
            const FString MemberSuffix = FormatSplitMemberSuffix(SourcePin);
            FString PinName = RootPin->PinName.ToString();
            if (PinName != UEdGraphSchema_K2::PN_ReturnValue.ToString() && PinName != *ExistingName)
            {
                const FString PinToken = FormatOutputPinNameToken(RootPin);
                return FString::Printf(TEXT("%%%s.%s%s"), **ExistingName, *PinToken, *MemberSuffix);
            }
            return FString::Printf(TEXT("%%%s%s"), **ExistingName, *MemberSuffix);
        }

        // Pure node that hasn't been emitted yet — this shouldn't happen if
        // EmitPureDependencies was called, but handle it gracefully
        if (!State.VisitedNodes.Contains(SourceNode))
        {
            // Emit it now as a pure node
            FString ValueName = State.AllocValueName();
            State.NodeToValueName.Add(SourceNode, ValueName);
            State.VisitedNodes.Add(SourceNode);

            auto ResolvePin = [this, &State](UEdGraphPin* Pin) -> FString
            {
                return ResolveInputValue(Pin, State);
            };

            FString PureLine = TextEmitter->EmitPureNode(SourceNode, ValueName, ResolvePin);
            AppendNodeLine(State, SourceNode, PureLine);

            UEdGraphPin* const RootPin = GetSplitPinRoot(SourcePin);
            const FString MemberSuffix = FormatSplitMemberSuffix(SourcePin);
            FString PinName = RootPin->PinName.ToString();
            if (PinName != UEdGraphSchema_K2::PN_ReturnValue.ToString() && !PinName.IsEmpty())
            {
                const FString PinToken = FormatOutputPinNameToken(RootPin);
                return FString::Printf(TEXT("%%%s.%s%s"), *ValueName, *PinToken, *MemberSuffix);
            }
            return FString::Printf(TEXT("%%%s%s"), *ValueName, *MemberSuffix);
        }

        // Source node was visited but has no value name (shouldn't normally happen)
        State.Warnings.Add(FBpirWarning{FString::Printf(
            TEXT("Unresolvable value: source node '%s' was visited but produced no value name"),
            *SourceNode->GetNodeTitle(ENodeTitleType::ListView).ToString()), EBpirWarningSeverity::Error});
        return FormatPinDefaultLiteral(InputPin);
    }

    // Case 2: pin has a default value
    FString DefaultValue = InputPin->DefaultValue;

    // PC_Text pins carry both DefaultValue (string form) and DefaultTextValue
    // (the FText with localization identity). When the FText has a namespace+key
    // we must emit NSLOCTEXT(...) so round-trip preserves the localization
    // metadata; checking DefaultValue first would short-circuit to a bare quoted
    // string and silently drop the identity. DefaultTextValue is the source of
    // truth for PC_Text — DefaultValue mirrors it for legacy/serialization.
    if (InputPin->PinType.PinCategory == UEdGraphSchema_K2::PC_Text
        && !InputPin->DefaultTextValue.IsEmpty())
    {
        if (InputPin->DefaultTextValue.IsFromStringTable())
        {
            FName TableId;
            FString TableKey;
            if (FTextInspector::GetTableIdAndKey(InputPin->DefaultTextValue, TableId, TableKey))
            {
                const FString LocTable = FString::Printf(
                    TEXT("LOCTABLE(\"%s\", \"%s\")"),
                    *BpirStructLiteralUtils::EscapeBpirStringInner(TableId.ToString()),
                    *BpirStructLiteralUtils::EscapeBpirStringInner(TableKey));
                return BpirStructLiteralUtils::EscapeBpirString(LocTable);
            }
        }

        const TOptional<FString> Namespace = FTextInspector::GetNamespace(InputPin->DefaultTextValue);
        const TOptional<FString> Key = FTextInspector::GetKey(InputPin->DefaultTextValue);
        const FString DisplayString = InputPin->DefaultTextValue.ToString();
        if (Namespace.IsSet() && !Namespace.GetValue().IsEmpty()
            && Key.IsSet() && !Key.GetValue().IsEmpty())
        {
            const FString NsLocText = FString::Printf(
                TEXT("NSLOCTEXT(\"%s\", \"%s\", \"%s\")"),
                *BpirStructLiteralUtils::EscapeBpirStringInner(Namespace.GetValue()),
                *BpirStructLiteralUtils::EscapeBpirStringInner(Key.GetValue()),
                *BpirStructLiteralUtils::EscapeBpirStringInner(DisplayString));
            return BpirStructLiteralUtils::EscapeBpirString(NsLocText);
        }
        // Identity-less FText: F-require-ftext-localization-identity makes the
        // compile path reject a bare quoted string. Synthesize a deterministic
        // <asset>/<nodeguid8>.<pin> identity so the BPIR round-trips through
        // the gate without hand-editing. The synthesized triple is stable
        // across re-decompiles of the same Blueprint (same asset name, same
        // node GUID, same pin name) so repeated round-trips are idempotent.
        const FString SynthLiteral = SynthesizeNSLocTextLiteral(InputPin, TargetBlueprint, DisplayString);
        return BpirStructLiteralUtils::EscapeBpirString(SynthLiteral);
    }

    if (!DefaultValue.IsEmpty())
    {
        // Format based on pin type
        const FName& Category = InputPin->PinType.PinCategory;

        if (Category == UEdGraphSchema_K2::PC_String
            || Category == UEdGraphSchema_K2::PC_Text
            || Category == UEdGraphSchema_K2::PC_FieldPath)
        {
            // Wrap strings in quotes, escape internal quotes
            FString Escaped = DefaultValue.Replace(TEXT("\\"), TEXT("\\\\")).Replace(TEXT("\""), TEXT("\\\""));
            return FString::Printf(TEXT("\"%s\""), *Escaped);
        }

        if (Category == UEdGraphSchema_K2::PC_Boolean)
        {
            return DefaultValue.ToLower();
        }

        if ((Category == UEdGraphSchema_K2::PC_Byte || Category == UEdGraphSchema_K2::PC_Enum)
            && InputPin->PinType.PinSubCategoryObject.IsValid())
        {
            // Enum value
            UEnum* EnumType = Cast<UEnum>(InputPin->PinType.PinSubCategoryObject.Get());
            if (EnumType)
            {
                FString EnumName = EnumType->GetName();
                FString ValueName = DefaultValue;

                // DefaultValue may be a numeric index (e.g. "0") — resolve to name
                if (DefaultValue.IsNumeric())
                {
                    int64 Index = FCString::Atoi64(*DefaultValue);
                    FString FullName = EnumType->GetNameStringByValue(Index);
                    if (!FullName.IsEmpty())
                    {
                        // GetNameStringByValue may return "EnumName::ValueName" or just "ValueName"
                        int32 ColonIdx = INDEX_NONE;
                        if (FullName.FindLastChar(TEXT(':'), ColonIdx) && ColonIdx + 1 < FullName.Len())
                        {
                            ValueName = FullName.Mid(ColonIdx + 1);
                        }
                        else
                        {
                            ValueName = FullName;
                        }
                    }
                }

                int32 ColonIdx = INDEX_NONE;
                if (ValueName.FindLastChar(TEXT(':'), ColonIdx) && ColonIdx + 1 < ValueName.Len())
                {
                    ValueName = ValueName.Mid(ColonIdx + 1);
                }

                return FString::Printf(TEXT("%s::%s"), *EnumName, *ValueName);
            }
        }
        else if ((Category == UEdGraphSchema_K2::PC_Byte || Category == UEdGraphSchema_K2::PC_Enum)
            && DefaultValue.StartsWith(TEXT(":")))
        {
            FString ValueName = DefaultValue;
            while (ValueName.StartsWith(TEXT(":")))
            {
                ValueName.RemoveFromStart(TEXT(":"));
            }
            const FString EnumName = InputPin->PinName.ToString();
            if (!EnumName.IsEmpty() && !ValueName.IsEmpty())
            {
                return FString::Printf(TEXT("%s::%s"), *EnumName, *ValueName);
            }
        }

        if (Category == UEdGraphSchema_K2::PC_Object || Category == UEdGraphSchema_K2::PC_SoftObject)
        {
            if (DefaultValue == TEXT("None") || DefaultValue.IsEmpty())
            {
                return TEXT("nullptr");
            }
        }

        // Struct category — convert Unreal native format to BPIR struct literals
        if (Category == UEdGraphSchema_K2::PC_Struct)
        {
            // Use PinSubCategoryObject to identify the exact struct type
            UScriptStruct* StructType = Cast<UScriptStruct>(InputPin->PinType.PinSubCategoryObject.Get());

            // Route the three sugared positional pin-default forms (FVector,
            // FRotator, FLinearColor) through the shared helper so node_props emit
            // and pin-default emit produce byte-identical output.
            if (StructType == TBaseStructure<FLinearColor>::Get()
                || StructType == TBaseStructure<FVector>::Get()
                || StructType == TBaseStructure<FRotator>::Get())
            {
                TArray<FString> Values = ExtractStructComponentValues(DefaultValue);
                const FString Sugared = BpirStructLiteralUtils::FormatStructComponentsAsBpir(StructType->GetName(), Values);
                if (!Sugared.IsEmpty())
                {
                    return Sugared;
                }
            }
            // Generic struct: pass through as-is (IsLiteral handles parenthesized format)
            return DefaultValue;
        }

        // Wildcard pins (e.g. MakeArray elements): treat as string literals
        if (Category == UEdGraphSchema_K2::PC_Wildcard)
        {
            return BpirStructLiteralUtils::EscapeBpirString(DefaultValue);
        }

        // FName pin defaults must be quoted on emit so the compile-side
        // resolver (FBpirValueResolver::IsLiteral) recognises the value as a
        // string literal instead of trying to dereference it as a variable.
        // Compiler-side FCodePinResolver::SetPinDefaultValue unquotes before
        // storing into the FName pin, so the round-trip is symmetric and
        // symbolic names like "None" survive intact. Parallel to the PC_String
        // branch above.
        if (Category == UEdGraphSchema_K2::PC_Name)
        {
            return BpirStructLiteralUtils::EscapeBpirString(DefaultValue);
        }

        // Numeric, etc. — return as-is
        return DefaultValue;
    }

    // Empty string/text defaults should emit "" not fall through to ?
    {
        const FName& Category = InputPin->PinType.PinCategory;
        if (Category == UEdGraphSchema_K2::PC_String
            || Category == UEdGraphSchema_K2::PC_Text
            || Category == UEdGraphSchema_K2::PC_FieldPath)
        {
            return TEXT("\"\"");
        }
    }

    // Check for default object
    if (InputPin->DefaultObject)
    {
        return InputPin->DefaultObject->GetPathName();
    }

    // Check for default text value
    if (!InputPin->DefaultTextValue.IsEmpty())
    {
        // String-table identity is carried separately from Namespace/Key; emit LOCTABLE first so the linkage survives recompile.
        if (InputPin->DefaultTextValue.IsFromStringTable())
        {
            FName TableId;
            FString TableKey;
            if (FTextInspector::GetTableIdAndKey(InputPin->DefaultTextValue, TableId, TableKey))
            {
                const FString LocTable = FString::Printf(
                    TEXT("LOCTABLE(\"%s\", \"%s\")"),
                    *BpirStructLiteralUtils::EscapeBpirStringInner(TableId.ToString()),
                    *BpirStructLiteralUtils::EscapeBpirStringInner(TableKey));
                return BpirStructLiteralUtils::EscapeBpirString(LocTable);
            }
        }

        // Preserve NSLOCTEXT namespace+key when present so localized BP defaults
        // round-trip through compile/decompile without silently un-localizing.
        // Bare quoted form remains for invariant-text cases (FText::FromString
        // with no namespace/key). Compile path's CoerceStringToPersistedFText
        // already routes NSLOCTEXT(...) through FTextStringHelper::CreateFromBuffer.
        const TOptional<FString> Namespace = FTextInspector::GetNamespace(InputPin->DefaultTextValue);
        const TOptional<FString> Key = FTextInspector::GetKey(InputPin->DefaultTextValue);
        const FString DisplayString = InputPin->DefaultTextValue.ToString();
        if (Namespace.IsSet() && !Namespace.GetValue().IsEmpty()
            && Key.IsSet() && !Key.GetValue().IsEmpty())
        {
            // Pre-escape the three substrings before formatting NSLOCTEXT(...).
            // The outer EscapeBpirString call below handles BPIR-level escaping of
            // the wrapping quotes, but the inner literal-format quotes around
            // namespace/key/display must be escaped here so that values containing
            // " or \\ produce a still-parseable NSLOCTEXT(...) string for
            // FTextStringHelper::CreateFromBuffer. EscapeBpirStringInner provides
            // the shared transform (backslash first, then quote) so this site
            // stays in lockstep with EscapeBpirString.
            const FString NsLocText = FString::Printf(
                TEXT("NSLOCTEXT(\"%s\", \"%s\", \"%s\")"),
                *BpirStructLiteralUtils::EscapeBpirStringInner(Namespace.GetValue()),
                *BpirStructLiteralUtils::EscapeBpirStringInner(Key.GetValue()),
                *BpirStructLiteralUtils::EscapeBpirStringInner(DisplayString));
            return BpirStructLiteralUtils::EscapeBpirString(NsLocText);
        }
        // Identity-less FText: same synthesis as the early PC_Text branch.
        // F-require-ftext-localization-identity rejects bare strings; the
        // synthesized <asset>/<nodeguid8>.<pin> identity keeps round-trip
        // re-compilable without changing the display string.
        const FString SynthLiteral = SynthesizeNSLocTextLiteral(InputPin, TargetBlueprint, DisplayString);
        return BpirStructLiteralUtils::EscapeBpirString(SynthLiteral);
    }

    // No value at all. FormatArgs already drops these pins from emitted call-site
    // arg lists (see IsPinOmittableAtCallSite in BpirTextEmitter.cpp), so this
    // path now only fires for non-arg positional contexts — branch conditions,
    // switch selectors, return values — where the caller still needs a token.
    // Render as a typed default literal (nullptr / "" / 0 / etc.). Only warn
    // when the helper falls through to <unresolved>; an unwired object pin
    // emitting nullptr is idiomatic, not a problem.
    const FString TypedDefault = FormatPinDefaultLiteral(InputPin);
    if (TypedDefault == TEXT("<unresolved>"))
    {
        State.Warnings.Add(FBpirWarning{FString::Printf(
            TEXT("Optional pin '%s' has no connection or default and no typed-default literal; emitted as <unresolved>."),
            *InputPin->PinName.ToString())});
    }
    return TypedDefault;
}

// ---------------------------------------------------------------------------
// Pure dependency emission
// ---------------------------------------------------------------------------

void FBpirDecompiler::EmitPureDependencies(UEdGraphNode* Node, FEntryState& State)
{
    if (!Node)
    {
        return;
    }

    // For each data input pin on the node, trace back to find unvisited pure nodes
    for (UEdGraphPin* Pin : Node->Pins)
    {
        if (!Pin) continue;
        if (Pin->Direction != EGPD_Input) continue;
        if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec) continue;
        if (Pin->LinkedTo.Num() == 0) continue;

        UEdGraphPin* SourcePin = Pin->LinkedTo[0];

        // Follow through reroute (knot) nodes. Pure-emit walker has no use for
        // dead-end-with-default values, so the OutDeadEndKnotInput is discarded.
        UEdGraphPin* UnusedDeadEnd = nullptr;
        SourcePin = BpirDecompiler::Helpers::FollowKnotsBackward(SourcePin, UnusedDeadEnd);
        UEdGraphNode* SourceNode = SourcePin ? SourcePin->GetOwningNode() : nullptr;
        if (!SourceNode) continue;

        // Skip nodes that are inlined (Self, VariableGet)
        if (SourceNode->IsA<UK2Node_Self>()) continue;
        if (SourceNode->IsA<UK2Node_VariableGet>()) continue;

        // Skip already-visited nodes (also serves as cycle guard for pure node chains)
        if (State.VisitedNodes.Contains(SourceNode)) continue;

        // Only emit pure nodes (no exec pins)
        bool bHasExecPin = false;
        for (UEdGraphPin* SrcPin : SourceNode->Pins)
        {
            if (SrcPin && SrcPin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
            {
                bHasExecPin = true;
                break;
            }
        }
        if (bHasExecPin) continue;

        // Mark visited BEFORE recursion to prevent infinite cycles in pure node chains
        State.VisitedNodes.Add(SourceNode);

        // Recursively emit dependencies of this pure node first
        EmitPureDependencies(SourceNode, State);

        // Now emit this pure node
        FString ValueName = State.AllocValueName();
        State.NodeToValueName.Add(SourceNode, ValueName);

        auto ResolvePin = [this, &State](UEdGraphPin* InPin) -> FString
        {
            return ResolveInputValue(InPin, State);
        };

        FString PureLine = TextEmitter->EmitPureNode(SourceNode, ValueName, ResolvePin);
        AppendNodeLine(State, SourceNode, PureLine);
    }
}

