// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Utils/BlueprintGraphSnapshot.h"


#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphSchema.h"
#include "Engine/Blueprint.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Logging/LogMacros.h"

DEFINE_LOG_CATEGORY_STATIC(LogBlueprintGraphSnapshot, Log, All);

namespace BlueprintGraphSnapshot
{
    namespace
    {
        void VisitPinRecursive(
            UEdGraphPin* Pin,
            TSet<UEdGraphPin*>& Visited,
            TFunctionRef<void(UEdGraphPin*)> Visitor)
        {
            if (!Pin || Visited.Contains(Pin))
            {
                return;
            }

            Visited.Add(Pin);
            Visitor(Pin);
            for (UEdGraphPin* SubPin : Pin->SubPins)
            {
                VisitPinRecursive(SubPin, Visited, Visitor);
            }
        }

        void ForEachPinRecursive(
            UEdGraphNode* Node,
            TFunctionRef<void(UEdGraphPin*)> Visitor)
        {
            if (!Node)
            {
                return;
            }

            TSet<UEdGraphPin*> Visited;
            for (UEdGraphPin* Pin : Node->Pins)
            {
                if (Pin && !Pin->ParentPin)
                {
                    VisitPinRecursive(Pin, Visited, Visitor);
                }
            }

            // A malformed or partially reconstructed node can contain a pin in
            // Node->Pins without a reachable parent chain. Still inspect it so
            // rollback never ignores a live LinkedTo array.
            for (UEdGraphPin* Pin : Node->Pins)
            {
                VisitPinRecursive(Pin, Visited, Visitor);
            }
        }

        bool BuildPinPath(const UEdGraphPin* Pin, TArray<FName>& OutPath)
        {
            OutPath.Reset();
            if (!Pin)
            {
                return false;
            }

            TArray<const UEdGraphPin*> ReversePath;
            TSet<const UEdGraphPin*> Visited;
            for (const UEdGraphPin* Current = Pin; Current; Current = Current->ParentPin)
            {
                if (Visited.Contains(Current))
                {
                    return false;
                }
                Visited.Add(Current);
                ReversePath.Add(Current);
            }

            OutPath.Reserve(ReversePath.Num());
            for (int32 Index = ReversePath.Num() - 1; Index >= 0; --Index)
            {
                OutPath.Add(ReversePath[Index]->PinName);
            }
            return OutPath.Num() > 0;
        }

        UEdGraphPin* FindPinByPath(
            UEdGraphNode* Node,
            const TArray<FName>& PinPath,
            EEdGraphPinDirection Direction)
        {
            if (!Node || PinPath.Num() == 0)
            {
                return nullptr;
            }

            UEdGraphPin* Current = nullptr;
            for (UEdGraphPin* Pin : Node->Pins)
            {
                if (Pin
                    && !Pin->ParentPin
                    && Pin->PinName == PinPath[0]
                    && Pin->Direction == Direction)
                {
                    Current = Pin;
                    break;
                }
            }

            for (int32 PathIndex = 1; Current && PathIndex < PinPath.Num(); ++PathIndex)
            {
                UEdGraphPin* MatchingSubPin = nullptr;
                for (UEdGraphPin* SubPin : Current->SubPins)
                {
                    if (SubPin
                        && SubPin->PinName == PinPath[PathIndex]
                        && SubPin->Direction == Direction)
                    {
                        MatchingSubPin = SubPin;
                        break;
                    }
                }
                Current = MatchingSubPin;
            }

            return Current;
        }

        FString PinPathToString(const TArray<FName>& PinPath)
        {
            TArray<FString> Names;
            Names.Reserve(PinPath.Num());
            for (const FName& Name : PinPath)
            {
                Names.Add(Name.ToString());
            }
            return FString::Join(Names, TEXT("."));
        }

        FString DescribeLink(const FBlueprintPinLinkSnapshot& Link)
        {
            return FString::Printf(TEXT("%s:%s[%d] -> %s:%s[%d]"),
                *Link.SourceNodeGuid.ToString(), *PinPathToString(Link.SourcePinPath),
                static_cast<int32>(Link.SourceDirection),
                *Link.LinkedNodeGuid.ToString(), *PinPathToString(Link.LinkedPinPath),
                static_cast<int32>(Link.LinkedDirection));
        }

        bool TryMakePinLinkSnapshot(
            const UEdGraphNode* FirstNode,
            const UEdGraphPin* FirstPin,
            const UEdGraphNode* SecondNode,
            const UEdGraphPin* SecondPin,
            FBlueprintPinLinkSnapshot& OutLink)
        {
            if (!FirstNode || !FirstPin || !SecondNode || !SecondPin)
            {
                return false;
            }

            TArray<FName> FirstPath;
            TArray<FName> SecondPath;
            if (!BuildPinPath(FirstPin, FirstPath) || !BuildPinPath(SecondPin, SecondPath))
            {
                return false;
            }

            // Canonicalize ordinary input/output links to the output endpoint so
            // the two reciprocal LinkedTo entries collapse into one set member.
            const bool bSwapEndpoints =
                FirstPin->Direction != EGPD_Output
                && SecondPin->Direction == EGPD_Output;
            if (bSwapEndpoints)
            {
                OutLink.SourceNodeGuid = SecondNode->NodeGuid;
                OutLink.SourcePinPath = MoveTemp(SecondPath);
                OutLink.SourceDirection = SecondPin->Direction;
                OutLink.LinkedNodeGuid = FirstNode->NodeGuid;
                OutLink.LinkedPinPath = MoveTemp(FirstPath);
                OutLink.LinkedDirection = FirstPin->Direction;
            }
            else
            {
                OutLink.SourceNodeGuid = FirstNode->NodeGuid;
                OutLink.SourcePinPath = MoveTemp(FirstPath);
                OutLink.SourceDirection = FirstPin->Direction;
                OutLink.LinkedNodeGuid = SecondNode->NodeGuid;
                OutLink.LinkedPinPath = MoveTemp(SecondPath);
                OutLink.LinkedDirection = SecondPin->Direction;
            }
            return true;
        }

        int32 CountLinksTo(const UEdGraphPin* Source, const UEdGraphPin* Target)
        {
            int32 Count = 0;
            if (!Source || !Target)
            {
                return Count;
            }
            for (const UEdGraphPin* LinkedPin : Source->LinkedTo)
            {
                Count += LinkedPin == Target ? 1 : 0;
            }
            return Count;
        }

        bool HasExactSymmetricLink(const UEdGraphPin* First, const UEdGraphPin* Second)
        {
            return CountLinksTo(First, Second) == 1
                && CountLinksTo(Second, First) == 1;
        }

        bool BreakLinkFromEitherEndpoint(UEdGraphPin* First, UEdGraphPin* Second)
        {
            if (!First || !Second)
            {
                return false;
            }

            const bool bFirstHasLink = First->LinkedTo.Contains(Second);
            const bool bSecondHasLink = Second->LinkedTo.Contains(First);
            if (bFirstHasLink && bSecondHasLink)
            {
                First->BreakLinkTo(Second);
                return true;
            }

            // BreakLinkTo intentionally ensures on one-sided link state. Repair
            // malformed asymmetry without triggering that ensure, while still
            // recording the pin modification and removing every duplicate.
            if (bFirstHasLink)
            {
                First->Modify();
                First->LinkedTo.Remove(Second);
            }
            if (bSecondHasLink)
            {
                Second->Modify();
                Second->LinkedTo.Remove(First);
            }
            return bFirstHasLink || bSecondHasLink;
        }

        bool ResolveLink(
            UBlueprint* BP,
            const FBlueprintPinLinkSnapshot& Link,
            UEdGraphPin*& OutSourcePin,
            UEdGraphPin*& OutLinkedPin,
            FString* OutError = nullptr)
        {
            OutSourcePin = nullptr;
            OutLinkedPin = nullptr;
            if (!BP)
            {
                if (OutError)
                {
                    *OutError = TEXT("Snapshot Blueprint is no longer valid.");
                }
                return false;
            }

            UEdGraphNode* SourceNode = FBlueprintEditorUtils::GetNodeByGUID(
                BP, Link.SourceNodeGuid);
            UEdGraphNode* LinkedNode = FBlueprintEditorUtils::GetNodeByGUID(
                BP, Link.LinkedNodeGuid);
            OutSourcePin = FindPinByPath(
                SourceNode, Link.SourcePinPath, Link.SourceDirection);
            OutLinkedPin = FindPinByPath(
                LinkedNode, Link.LinkedPinPath, Link.LinkedDirection);
            if (OutSourcePin && OutLinkedPin)
            {
                return true;
            }

            if (OutError)
            {
                *OutError = FString::Printf(
                    TEXT("Could not resolve captured Blueprint link %s."),
                    *DescribeLink(Link));
            }
            return false;
        }

        void CaptureCollection(const TArray<UEdGraph*>& Source,
            TMap<TWeakObjectPtr<UEdGraph>, TSet<TWeakObjectPtr<UEdGraphNode>>>& OutNodesByGraph,
            TSet<FBlueprintPinLinkSnapshot>& OutPinLinks)
        {
            for (UEdGraph* Graph : Source)
            {
                if (!Graph)
                {
                    continue;
                }

                TSet<TWeakObjectPtr<UEdGraphNode>> NodeSet;
                NodeSet.Reserve(Graph->Nodes.Num());
                for (UEdGraphNode* Node : Graph->Nodes)
                {
                    if (!Node)
                    {
                        continue;
                    }
                    NodeSet.Add(Node);

                    ForEachPinRecursive(Node, [&](UEdGraphPin* Pin)
                    {
                        for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
                        {
                            UEdGraphNode* LinkedNode = LinkedPin
                                ? LinkedPin->GetOwningNodeUnchecked()
                                : nullptr;
                            FBlueprintPinLinkSnapshot Link;
                            if (TryMakePinLinkSnapshot(
                                    Node, Pin, LinkedNode, LinkedPin, Link))
                            {
                                OutPinLinks.Add(MoveTemp(Link));
                            }
                        }
                    });
                }
                OutNodesByGraph.Add(Graph, MoveTemp(NodeSet));
            }
        }

        void RestorePinLinks(UBlueprint* BP,
            const FBlueprintGraphSnapshot& Snapshot,
            bool& bOutLinksChanged)
        {
            if (!BP)
            {
                return;
            }

            // Remove links that did not exist in the pre-image. Newly created
            // nodes have already been removed above, but a failed compile can
            // also leave a surviving anchor pin wired to a temporary node.
            TArray<UEdGraph*> CurrentGraphs;
            BP->GetAllGraphs(CurrentGraphs);
            for (UEdGraph* Graph : CurrentGraphs)
            {
                if (!Graph)
                {
                    continue;
                }

                for (UEdGraphNode* Node : Graph->Nodes)
                {
                    if (!Node)
                    {
                        continue;
                    }

                    ForEachPinRecursive(Node, [&](UEdGraphPin* Pin)
                    {
                        const TArray<UEdGraphPin*> CurrentLinkedPins = Pin->LinkedTo;
                        for (UEdGraphPin* LinkedPin : CurrentLinkedPins)
                        {
                            if (!LinkedPin)
                            {
                                Pin->Modify();
                                bOutLinksChanged |= Pin->LinkedTo.Remove(nullptr) > 0;
                                continue;
                            }

                            UEdGraphNode* LinkedNode = LinkedPin
                                ->GetOwningNodeUnchecked();
                            FBlueprintPinLinkSnapshot CurrentLink;
                            if ((!TryMakePinLinkSnapshot(
                                    Node, Pin, LinkedNode, LinkedPin, CurrentLink)
                                    || !Snapshot.PinLinks.Contains(CurrentLink))
                                && Pin->LinkedTo.Contains(LinkedPin))
                            {
                                bOutLinksChanged |=
                                    BreakLinkFromEitherEndpoint(Pin, LinkedPin);
                            }
                        }
                    });
                }
            }

            // Add any pre-image links that the transaction undo did not restore.
            for (const FBlueprintPinLinkSnapshot& Link : Snapshot.PinLinks)
            {
                UEdGraphPin* SourcePin = nullptr;
                UEdGraphPin* LinkedPin = nullptr;
                FString ResolveError;
                if (!ResolveLink(BP, Link, SourcePin, LinkedPin, &ResolveError))
                {
                    UE_LOG(LogBlueprintGraphSnapshot, Warning,
                        TEXT("%s"), *ResolveError);
                    continue;
                }

                if (HasExactSymmetricLink(SourcePin, LinkedPin))
                {
                    continue;
                }

                bOutLinksChanged = true;
                BreakLinkFromEitherEndpoint(SourcePin, LinkedPin);

                const UEdGraphSchema* Schema = SourcePin->GetSchema();
                if (Schema)
                {
                    Schema->TryCreateConnection(SourcePin, LinkedPin);
                }

                // A captured pre-image is authoritative. If the current schema
                // refuses it (for example while pins are mid-reconstruction),
                // fall back to the pin API after clearing any one-sided state.
                if (!HasExactSymmetricLink(SourcePin, LinkedPin))
                {
                    BreakLinkFromEitherEndpoint(SourcePin, LinkedPin);
                    SourcePin->MakeLinkTo(LinkedPin);
                }

                if (!HasExactSymmetricLink(SourcePin, LinkedPin))
                {
                    UE_LOG(LogBlueprintGraphSnapshot, Warning,
                        TEXT("Could not restore captured Blueprint link %s."),
                        *DescribeLink(Link));
                }
            }
        }

        // Walks a current graph collection and removes any graph whose pointer is
        // not a key in the snapshot's NodesByGraph map. Returns the number of
        // graphs removed.
        int32 RollbackGraphCollection(UBlueprint* BP,
            const TArray<TObjectPtr<UEdGraph>>& Current,
            const FBlueprintGraphSnapshot& Snapshot)
        {
            // Copy first — RemoveGraph mutates the underlying array.
            TArray<UEdGraph*> ToRemove;
            for (const TObjectPtr<UEdGraph>& GraphPtr : Current)
            {
                UEdGraph* Graph = GraphPtr.Get();
                if (Graph && !Snapshot.NodesByGraph.Contains(Graph))
                {
                    ToRemove.Add(Graph);
                }
            }

            for (UEdGraph* Graph : ToRemove)
            {
                FBlueprintEditorUtils::RemoveGraph(BP, Graph, EGraphRemoveFlags::None);
            }

            return ToRemove.Num();
        }
    }

    FBlueprintGraphSnapshot Capture(UBlueprint* Blueprint)
    {
        FBlueprintGraphSnapshot Snapshot;
        if (!Blueprint)
        {
            return Snapshot;
        }

        Snapshot.Blueprint = Blueprint;
        TArray<UEdGraph*> AuthoredGraphs;
        Blueprint->GetAllGraphs(AuthoredGraphs);
        Snapshot.NodesByGraph.Reserve(AuthoredGraphs.Num());
        CaptureCollection(AuthoredGraphs, Snapshot.NodesByGraph, Snapshot.PinLinks);
        return Snapshot;
    }

    bool VerifyLinkTopology(const FBlueprintGraphSnapshot& Snapshot, FString& OutError)
    {
        OutError.Reset();
        UBlueprint* BP = Snapshot.Blueprint.Get();
        if (!BP)
        {
            OutError = TEXT("Snapshot Blueprint is no longer valid.");
            return false;
        }

        TSet<FBlueprintPinLinkSnapshot> CurrentLinks;
        TArray<UEdGraph*> CurrentGraphs;
        BP->GetAllGraphs(CurrentGraphs);
        for (UEdGraph* Graph : CurrentGraphs)
        {
            if (!Graph)
            {
                continue;
            }

            for (UEdGraphNode* Node : Graph->Nodes)
            {
                ForEachPinRecursive(Node, [&](UEdGraphPin* Pin)
                {
                    if (!OutError.IsEmpty())
                    {
                        return;
                    }

                    for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
                    {
                        UEdGraphNode* LinkedNode = LinkedPin
                            ? LinkedPin->GetOwningNodeUnchecked()
                            : nullptr;
                        FBlueprintPinLinkSnapshot CurrentLink;
                        if (!TryMakePinLinkSnapshot(
                                Node, Pin, LinkedNode, LinkedPin, CurrentLink))
                        {
                            OutError = FString::Printf(
                                TEXT("Blueprint link on node '%s' has an invalid endpoint or pin path."),
                                *Node->NodeGuid.ToString());
                            return;
                        }
                        CurrentLinks.Add(MoveTemp(CurrentLink));
                    }
                });

                if (!OutError.IsEmpty())
                {
                    return false;
                }
            }
        }

        for (const FBlueprintPinLinkSnapshot& CurrentLink : CurrentLinks)
        {
            if (!Snapshot.PinLinks.Contains(CurrentLink))
            {
                OutError = FString::Printf(
                    TEXT("Rollback left an unexpected Blueprint link %s."),
                    *DescribeLink(CurrentLink));
                return false;
            }
        }

        for (const FBlueprintPinLinkSnapshot& SavedLink : Snapshot.PinLinks)
        {
            UEdGraphPin* SourcePin = nullptr;
            UEdGraphPin* LinkedPin = nullptr;
            if (!ResolveLink(BP, SavedLink, SourcePin, LinkedPin, &OutError))
            {
                return false;
            }
            if (!HasExactSymmetricLink(SourcePin, LinkedPin))
            {
                OutError = FString::Printf(
                    TEXT("Rollback did not restore captured Blueprint link exactly once in both directions: %s."),
                    *DescribeLink(SavedLink));
                return false;
            }
        }

        return true;
    }

    int32 RollbackToSnapshot(const FBlueprintGraphSnapshot& Snapshot)
    {
        UBlueprint* BP = Snapshot.Blueprint.Get();
        if (!BP)
        {
            return 0;
        }

        // Phase 1: drop any graph whose pointer is not in the snapshot. Iteration
        // order is deterministic: Ubergraph → Function → Macro.
        int32 RemovedGraphs = 0;
        RemovedGraphs += RollbackGraphCollection(BP, BP->UbergraphPages, Snapshot);
        RemovedGraphs += RollbackGraphCollection(BP, BP->FunctionGraphs, Snapshot);
        RemovedGraphs += RollbackGraphCollection(BP, BP->MacroGraphs, Snapshot);

        // Phase 2: for each graph that was in the snapshot and is still alive,
        // drop any node not in its captured node set. Iterating the snapshot map
        // directly is sufficient — every graph that mattered is a key here.
        int32 RemovedNodes = 0;
        for (const TPair<TWeakObjectPtr<UEdGraph>, TSet<TWeakObjectPtr<UEdGraphNode>>>& Pair : Snapshot.NodesByGraph)
        {
            UEdGraph* Graph = Pair.Key.Get();
            if (!Graph)
            {
                continue;
            }

            const TSet<TWeakObjectPtr<UEdGraphNode>>& SnapshotNodes = Pair.Value;

            TArray<UEdGraphNode*> NodesToRemove;
            for (UEdGraphNode* Node : Graph->Nodes)
            {
                if (!Node)
                {
                    continue;
                }
                if (!SnapshotNodes.Contains(Node))
                {
                    NodesToRemove.Add(Node);
                }
            }

            for (UEdGraphNode* Node : NodesToRemove)
            {
                FBlueprintEditorUtils::RemoveNode(BP, Node, /*bDontRecompile=*/true);
                ++RemovedNodes;
            }
        }

        bool bLinksChanged = false;
        RestorePinLinks(BP, Snapshot, bLinksChanged);

        const int32 RemovedTotal = RemovedGraphs + RemovedNodes;
        if (RemovedTotal > 0 || bLinksChanged)
        {
            FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
            UE_LOG(LogBlueprintGraphSnapshot, Verbose,
                TEXT("Rolled back %d node(s), %d graph(s); pin links changed=%s on '%s'."),
                RemovedNodes, RemovedGraphs, bLinksChanged ? TEXT("true") : TEXT("false"),
                *BP->GetName());
        }

        return RemovedTotal;
    }
}
