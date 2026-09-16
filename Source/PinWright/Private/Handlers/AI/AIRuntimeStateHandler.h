// Copyright (c) 2026 Alexander Penkin. MIT License.

// Pure state-describing seams behind ai.get_runtime_state.
//
// They are a named namespace rather than file-local statics for two reasons: the module
// is a unity build (file-local helpers with common names collide across merged TUs), and
// the automation tests drive these functions directly on components constructed in a test
// world, which is the only part of the verb reachable without a live PIE session.
//
// Every function reads ONLY public engine API (UBrainComponent / UBehaviorTreeComponent /
// UPathFollowingComponent / UBlackboardComponent accessors). None of them writes, ticks,
// or resolves anything - passing a null component yields a JSON object that says so via
// `reason` rather than an empty or zero-filled one, which is the whole point of the verb:
// a caller reaching for GetCurrentAcceleration() as a substitute reads (0,0,0) on a pawn
// that is plainly moving, because AI path following drives it through RequestDirectMove.
#pragma once

#include "CoreMinimal.h"

class FJsonObject;
class UBehaviorTreeComponent;
class UBlackboardComponent;
class UBrainComponent;
class UPathFollowingComponent;

namespace AIRuntimeState
{
    // Brain section: class, running/paused, and - for a UBehaviorTreeComponent brain - the
    // active tree, the active node with its class/name/execution index, the root-first node
    // path, the active task with its EBTTaskStatus, and the engine's own DescribeActiveTasks
    // / DescribeActiveTrees strings. A non-BT brain (StateTree, a custom UBrainComponent)
    // reports isBehaviorTree=false and a `behaviorTree` object carrying present:false plus a
    // reason, instead of pretending the BT fields are absent because the tree is idle.
    TSharedPtr<FJsonObject> DescribeBrain(const UBrainComponent* Brain);

    // Path-following section: GetStatus()/GetStatusDesc(), hasValidPath, the current request
    // id, acceptance radius, goal actor, and - only when a path is actually valid - the
    // current segment target and the path's end location. didMoveReachGoal is the engine's
    // one public readback of the last move's outcome; the raw EPathFollowingResult is NOT
    // publicly stored on the component, so lastMoveResult is reported as null with a reason
    // rather than fabricated from status.
    TSharedPtr<FJsonObject> DescribePathFollowing(const UPathFollowingComponent* PathFollowing);

    // Blackboard section: the asset, the key count, and every key as
    // {name, type, value} where value is UBlackboardComponent::DescribeKeyValue in
    // EBlackboardDescription::OnlyValue mode (the same text the Gameplay Debugger shows).
    // A component with no asset bound reports keyCount 0 and an empty keys array plus a
    // reason, because GetNumKeys() also returns 0 for a fully-populated-but-unbound one.
    TSharedPtr<FJsonObject> DescribeBlackboard(const UBlackboardComponent* Blackboard);
}
