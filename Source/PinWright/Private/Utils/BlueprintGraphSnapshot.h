// Copyright (c) 2026 Alexander Penkin. MIT License.

// FBlueprintGraphSnapshot — value-typed pre-image of a UBlueprint's authored graph
// state. Captures every graph returned by UBlueprint::GetAllGraphs, including
// implemented-interface and child graphs, so callers can roll back mid-operation
// mutations that bypass the transaction buffer (e.g. node creations via
// NewObject + Graph->AddNode that never call Modify()). Snapshot rollback runs
// after PinWrightTransactionUtils::ApplyAndCancelTransaction has
// replayed property-level diffs; together they cover both transactional and
// non-transactional graph mutations.
#pragma once

#include "CoreMinimal.h"
#include "EdGraph/EdGraphPin.h"
#include "UObject/WeakObjectPtr.h"

class UBlueprint;
class UEdGraph;
class UEdGraphNode;

namespace BlueprintGraphSnapshot
{
    struct FBlueprintPinLinkSnapshot
    {
        FGuid SourceNodeGuid;
        TArray<FName> SourcePinPath;
        EEdGraphPinDirection SourceDirection = EGPD_Output;
        FGuid LinkedNodeGuid;
        TArray<FName> LinkedPinPath;
        EEdGraphPinDirection LinkedDirection = EGPD_Input;

        bool operator==(const FBlueprintPinLinkSnapshot& Other) const
        {
            return SourceNodeGuid == Other.SourceNodeGuid
                && SourcePinPath == Other.SourcePinPath
                && SourceDirection == Other.SourceDirection
                && LinkedNodeGuid == Other.LinkedNodeGuid
                && LinkedPinPath == Other.LinkedPinPath
                && LinkedDirection == Other.LinkedDirection;
        }
    };

    inline uint32 GetTypeHash(const FBlueprintPinLinkSnapshot& Link)
    {
        uint32 Hash = GetTypeHash(Link.SourceNodeGuid);
        for (const FName& PinName : Link.SourcePinPath)
        {
            Hash = HashCombine(Hash, GetTypeHash(PinName));
        }
        Hash = HashCombine(Hash, ::GetTypeHash(static_cast<uint8>(Link.SourceDirection)));
        Hash = HashCombine(Hash, GetTypeHash(Link.LinkedNodeGuid));
        for (const FName& PinName : Link.LinkedPinPath)
        {
            Hash = HashCombine(Hash, GetTypeHash(PinName));
        }
        return HashCombine(Hash, ::GetTypeHash(static_cast<uint8>(Link.LinkedDirection)));
    }

    struct FBlueprintGraphSnapshot
    {
        TWeakObjectPtr<UBlueprint> Blueprint;
        // Authoritative pre-image: every captured authored graph appears exactly
        // once as a key, with its node set as the value. Membership lookup is
        // O(1) via TMap::Contains.
        TMap<TWeakObjectPtr<UEdGraph>, TSet<TWeakObjectPtr<UEdGraphNode>>> NodesByGraph;
        // Connections between surviving nodes, represented by stable node GUIDs
        // and recursive pin paths so split/sub-pin links are restored exactly.
        TSet<FBlueprintPinLinkSnapshot> PinLinks;

        bool IsValid() const { return Blueprint.IsValid(); }
    };

    FBlueprintGraphSnapshot Capture(UBlueprint* Blueprint);

    // Removes newly added graphs from UbergraphPages, FunctionGraphs, and
    // MacroGraphs, and newly added nodes from all captured graphs. Returns the
    // total number removed (graphs + nodes). Restores the captured pin links on
    // surviving nodes as part of the same rollback. Marks the blueprint
    // structurally modified iff graph/node membership or pin links changed.
    int32 RollbackToSnapshot(const FBlueprintGraphSnapshot& Snapshot);

    // Verifies that every captured link resolves and exists exactly once in both
    // endpoint LinkedTo arrays, with no post-snapshot links left behind.
    bool VerifyLinkTopology(const FBlueprintGraphSnapshot& Snapshot, FString& OutError);
}
