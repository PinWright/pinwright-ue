// Copyright (c) 2026 Alexander Penkin. MIT License.

// ai.get_runtime_state - the only read of a RUNNING AI in the plugin.
//
// Everything else under `ai` and `behavior_tree` reads or writes ASSETS. This verb reads the
// controller executing them: which Behavior Tree node is active, what path following is doing,
// and the live Blackboard. Those three facts are what separate "the pawn is stuck" from "a
// decorator keeps aborting and restarting the branch", and neither is derivable from position
// samples taken across separate RPC calls.

#include "Handlers/AI/AIRuntimeStateHandler.h"

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ParamSpec.h"
#include "Handlers/ErrorCodes.h"
#include "Handlers/Actor/ActorNameParamUtils.h"
#include "Utils/ActorUtils.h"
#include "Utils/JsonBuilders.h"

#include "AIController.h"
#include "BrainComponent.h"
#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/BehaviorTreeComponent.h"
#include "BehaviorTree/BehaviorTreeTypes.h"
#include "BehaviorTree/BlackboardComponent.h"
#include "BehaviorTree/BlackboardData.h"
#include "BehaviorTree/BTNode.h"
#include "BehaviorTree/BTTaskNode.h"
#include "Navigation/PathFollowingComponent.h"
#include "NavigationData.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/World.h"
#include "GameFramework/Controller.h"
#include "GameFramework/Pawn.h"

namespace
{
    // Every optional section of the response is an object carrying `present`, so an absent
    // one is a stated fact with a reason rather than a missing key the caller has to guess
    // about. A zero-filled or omitted section is exactly the failure this verb exists to
    // stop: a caller with no status field reaches for GetCurrentAcceleration(), reads
    // (0,0,0) on a pawn that is plainly moving, and concludes the mover is not trying.
    TSharedPtr<FJsonObject> AiRuntimeAbsentSection(const FString& Reason)
    {
        TSharedPtr<FJsonObject> Section = MakeShared<FJsonObject>();
        Section->SetBoolField(TEXT("present"), false);
        Section->SetStringField(TEXT("reason"), Reason);
        return Section;
    }

    TSharedPtr<FJsonObject> AiRuntimePresentSection()
    {
        TSharedPtr<FJsonObject> Section = MakeShared<FJsonObject>();
        Section->SetBoolField(TEXT("present"), true);
        return Section;
    }

    void AiRuntimeSetNull(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field,
        const TCHAR* ReasonField, const FString& Reason)
    {
        Object->SetField(Field, MakeShared<FJsonValueNull>());
        Object->SetStringField(ReasonField, Reason);
    }

    FString AiRuntimeObjectPath(const UObject* Object)
    {
        return Object ? Object->GetPathName() : FString();
    }

    // EBTTaskStatus is a plain namespaced enum (no UENUM), so there is no reflection to
    // resolve a name from. Three values, spelled out rather than indexed into a table.
    const TCHAR* AiRuntimeTaskStatusName(EBTTaskStatus::Type Status)
    {
        switch (Status)
        {
        case EBTTaskStatus::Active:   return TEXT("Active");
        case EBTTaskStatus::Aborting: return TEXT("Aborting");
        case EBTTaskStatus::Inactive: return TEXT("Inactive");
        default:                      return TEXT("Unknown");
        }
    }

    TSharedPtr<FJsonObject> AiRuntimeDescribeNode(const UBTNode* Node)
    {
        TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
        Json->SetStringField(TEXT("name"), Node->GetNodeName());
        Json->SetStringField(TEXT("class"), Node->GetClass()->GetName());
        Json->SetStringField(TEXT("classPath"), Node->GetClass()->GetPathName());
        Json->SetNumberField(TEXT("executionIndex"), Node->GetExecutionIndex());
        Json->SetNumberField(TEXT("treeDepth"), Node->GetTreeDepth());
        return Json;
    }

    // The BT half of the brain section. Split out so DescribeBrain stays a dispatch over
    // brain kinds and this stays the one place that knows BehaviorTreeComponent's API.
    TSharedPtr<FJsonObject> AiRuntimeDescribeBehaviorTree(const UBehaviorTreeComponent* BTComp)
    {
        if (!BTComp->TreeHasBeenStarted())
        {
            return AiRuntimeAbsentSection(TEXT("The Behavior Tree component has no started tree ")
                TEXT("(StartTree has not run, or StopTree cleared the instance stack). ")
                TEXT("Assign a tree with ai.assign_behavior_tree and possess the pawn."));
        }

        TSharedPtr<FJsonObject> Json = AiRuntimePresentSection();
        Json->SetStringField(TEXT("currentTree"), AiRuntimeObjectPath(BTComp->GetCurrentTree()));
        Json->SetStringField(TEXT("rootTree"), AiRuntimeObjectPath(BTComp->GetRootTree()));
        Json->SetNumberField(TEXT("activeInstanceIndex"), BTComp->GetActiveInstanceIdx());
        Json->SetStringField(TEXT("activeTrees"), BTComp->DescribeActiveTrees());
        Json->SetStringField(TEXT("activeTasks"), BTComp->DescribeActiveTasks());

        const UBTNode* ActiveNode = BTComp->GetActiveNode();
        if (!ActiveNode)
        {
            Json->SetObjectField(TEXT("activeNode"),
                AiRuntimeAbsentSection(TEXT("The active instance has no active node this frame.")));
            Json->SetArrayField(TEXT("activeNodePath"), TArray<TSharedPtr<FJsonValue>>());
            return Json;
        }

        TSharedPtr<FJsonObject> ActiveNodeJson = AiRuntimeDescribeNode(ActiveNode);
        ActiveNodeJson->SetBoolField(TEXT("present"), true);
        ActiveNodeJson->SetStringField(TEXT("staticDescription"), ActiveNode->GetStaticDescription());

        // GetActiveNode() is whatever the active instance is sitting on: a task while one runs,
        // otherwise the composite the search settled on. Reporting taskStatus only for a task
        // keeps "no task is running" distinguishable from "a task is Inactive".
        if (const UBTTaskNode* ActiveTask = Cast<UBTTaskNode>(ActiveNode))
        {
            ActiveNodeJson->SetBoolField(TEXT("isTask"), true);
            ActiveNodeJson->SetStringField(TEXT("taskStatus"),
                AiRuntimeTaskStatusName(BTComp->GetTaskStatus(ActiveTask)));
        }
        else
        {
            ActiveNodeJson->SetBoolField(TEXT("isTask"), false);
            AiRuntimeSetNull(ActiveNodeJson, TEXT("taskStatus"), TEXT("taskStatusReason"),
                TEXT("The active node is not a UBTTaskNode, so it has no EBTTaskStatus."));
        }
        Json->SetObjectField(TEXT("activeNode"), ActiveNodeJson);

        // Root-first path, walked up through the public parent chain the same way
        // UBehaviorTreeComponent::GetDebugInfoString does, then reversed.
        TArray<TSharedPtr<FJsonValue>> PathToRoot;
        for (const UBTNode* Node = ActiveNode; Node != nullptr; Node = Node->GetParentNode())
        {
            PathToRoot.Insert(MakeShared<FJsonValueObject>(AiRuntimeDescribeNode(Node)), 0);
        }
        Json->SetArrayField(TEXT("activeNodePath"), PathToRoot);
        return Json;
    }
}

namespace AIRuntimeState
{

TSharedPtr<FJsonObject> DescribeBrain(const UBrainComponent* Brain)
{
    if (!Brain)
    {
        return AiRuntimeAbsentSection(TEXT("The controller carries no UBrainComponent, so no ")
            TEXT("Behavior Tree or StateTree logic is running on it."));
    }

    TSharedPtr<FJsonObject> Json = AiRuntimePresentSection();
    Json->SetStringField(TEXT("componentClass"), Brain->GetClass()->GetName());
    Json->SetStringField(TEXT("componentPath"), Brain->GetPathName());
    Json->SetBoolField(TEXT("isRunning"), Brain->IsRunning());
    Json->SetBoolField(TEXT("isPaused"), Brain->IsPaused());

    const UBehaviorTreeComponent* BTComp = Cast<UBehaviorTreeComponent>(Brain);
    Json->SetBoolField(TEXT("isBehaviorTree"), BTComp != nullptr);
    Json->SetObjectField(TEXT("behaviorTree"), BTComp
        ? AiRuntimeDescribeBehaviorTree(BTComp)
        : AiRuntimeAbsentSection(FString::Printf(
            TEXT("The brain is a %s, not a UBehaviorTreeComponent, so it has no active BT node."),
            *Brain->GetClass()->GetName())));
    return Json;
}

TSharedPtr<FJsonObject> DescribePathFollowing(const UPathFollowingComponent* PathFollowing)
{
    if (!PathFollowing)
    {
        return AiRuntimeAbsentSection(TEXT("The controller carries no UPathFollowingComponent, so ")
            TEXT("it issues no MoveTo requests and has no move status."));
    }

    TSharedPtr<FJsonObject> Json = AiRuntimePresentSection();
    Json->SetStringField(TEXT("componentClass"), PathFollowing->GetClass()->GetName());
    Json->SetStringField(TEXT("componentPath"), PathFollowing->GetPathName());
    Json->SetStringField(TEXT("status"), PathFollowing->GetStatusDesc());
    Json->SetNumberField(TEXT("statusValue"), static_cast<int32>(PathFollowing->GetStatus()));
    Json->SetBoolField(TEXT("hasValidPath"), PathFollowing->HasValidPath());
    Json->SetBoolField(TEXT("hasPartialPath"), PathFollowing->HasPartialPath());
    Json->SetBoolField(TEXT("didMoveReachGoal"), PathFollowing->DidMoveReachGoal());
    Json->SetNumberField(TEXT("currentRequestId"),
        static_cast<double>(PathFollowing->GetCurrentRequestId().GetID()));
    Json->SetBoolField(TEXT("currentRequestIdValid"), PathFollowing->GetCurrentRequestId().IsValid());
    Json->SetNumberField(TEXT("acceptanceRadius"), PathFollowing->GetAcceptanceRadius());
    Json->SetNumberField(TEXT("currentPathIndex"), static_cast<double>(PathFollowing->GetCurrentPathIndex()));
    Json->SetNumberField(TEXT("nextPathIndex"), static_cast<double>(PathFollowing->GetNextPathIndex()));

    if (const AActor* GoalActor = PathFollowing->GetMoveGoal())
    {
        Json->SetStringField(TEXT("goalActorPath"), GoalActor->GetPathName());
    }
    else
    {
        AiRuntimeSetNull(Json, TEXT("goalActorPath"), TEXT("goalActorReason"),
            TEXT("The move request targets a location, not an actor."));
    }

    // Both locations are read only behind HasValidPath: with no path, CurrentDestination and
    // the point array hold whatever the previous request left, and reporting that as the
    // current goal is the fabricated answer this verb exists to avoid.
    if (PathFollowing->HasValidPath())
    {
        Json->SetObjectField(TEXT("currentTargetLocation"),
            JsonBuilders::BuildVectorJson(PathFollowing->GetCurrentTargetLocation()));
        Json->SetObjectField(TEXT("pathEndLocation"),
            JsonBuilders::BuildVectorJson(PathFollowing->GetPath()->GetEndLocation()));
    }
    else
    {
        const FString NoPath = TEXT("No valid path is set, so the stored destination is stale.");
        AiRuntimeSetNull(Json, TEXT("currentTargetLocation"), TEXT("currentTargetLocationReason"), NoPath);
        AiRuntimeSetNull(Json, TEXT("pathEndLocation"), TEXT("pathEndLocationReason"), NoPath);
    }

    // UPathFollowingComponent keeps the last FPathFollowingResult in a protected member and
    // exposes no getter; DidMoveReachGoal() (above) is the whole public readback. Emitting a
    // null with the reason is honest, where deriving a result from Status would not be.
    AiRuntimeSetNull(Json, TEXT("lastMoveResult"), TEXT("lastMoveResultReason"),
        TEXT("UPathFollowingComponent exposes no public accessor for the last ")
        TEXT("EPathFollowingResult; didMoveReachGoal is the only readback of the last move. ")
        TEXT("Bind OnRequestFinished in game code to capture the result code itself."));

    return Json;
}

TSharedPtr<FJsonObject> DescribeBlackboard(const UBlackboardComponent* Blackboard)
{
    if (!Blackboard)
    {
        return AiRuntimeAbsentSection(TEXT("The controller carries no UBlackboardComponent."));
    }

    TSharedPtr<FJsonObject> Json = AiRuntimePresentSection();
    Json->SetStringField(TEXT("componentClass"), Blackboard->GetClass()->GetName());
    Json->SetStringField(TEXT("componentPath"), Blackboard->GetPathName());

    const UBlackboardData* Asset = Blackboard->GetBlackboardAsset();
    if (!Asset)
    {
        // GetNumKeys() returns 0 for an unbound component too, so an empty keys array without
        // this reason would read as "the Blackboard has no keys".
        AiRuntimeSetNull(Json, TEXT("assetPath"), TEXT("assetReason"),
            TEXT("No UBlackboardData is bound to the component, so it holds no keys at all."));
        Json->SetNumberField(TEXT("keyCount"), 0);
        Json->SetArrayField(TEXT("keys"), TArray<TSharedPtr<FJsonValue>>());
        return Json;
    }

    Json->SetStringField(TEXT("assetPath"), Asset->GetPathName());
    const int32 NumKeys = Blackboard->GetNumKeys();
    Json->SetNumberField(TEXT("keyCount"), NumKeys);

    TArray<TSharedPtr<FJsonValue>> Keys;
    Keys.Reserve(NumKeys);
    for (int32 Index = 0; Index < NumKeys; ++Index)
    {
        const FBlackboard::FKey KeyID(Index);
        TSharedPtr<FJsonObject> KeyJson = MakeShared<FJsonObject>();
        KeyJson->SetStringField(TEXT("name"), Blackboard->GetKeyName(KeyID).ToString());
        const UClass* KeyTypeClass = Blackboard->GetKeyType(KeyID).Get();
        KeyJson->SetStringField(TEXT("type"),
            KeyTypeClass ? KeyTypeClass->GetName() : FString(TEXT("Unknown")));
        KeyJson->SetStringField(TEXT("value"),
            Blackboard->DescribeKeyValue(KeyID, EBlackboardDescription::OnlyValue));
        Keys.Add(MakeShared<FJsonValueObject>(KeyJson));
    }
    Json->SetArrayField(TEXT("keys"), Keys);
    return Json;
}

} // namespace AIRuntimeState

// ---- ai.get_runtime_state ----
REGISTER_RPC_HANDLER("ai.get_runtime_state", "ai",
    "Read the live state of a RUNNING AI during PIE: the brain component and whether it is "
    "running/paused; for a Behavior Tree brain the active tree, the active node (class, name, "
    "execution index) with its root-first path and, when the active node is a task, its "
    "EBTTaskStatus; path-following status (GetStatus/GetStatusDesc), hasValidPath, current move "
    "request id, goal actor or goal location, and didMoveReachGoal; plus a compact live "
    "Blackboard key dump. Read-only, one actor per call. Accepts either a possessed Pawn or the "
    "AIController itself. Requires an active PIE session (errors NOT_IN_PIE); errors "
    "NO_AI_CONTROLLER when the target has no controller and NO_BRAIN_COMPONENT when the "
    "controller runs neither a brain nor path following. Each of the brain / pathFollowing / "
    "blackboard sections carries present:true|false with a reason, and unreachable fields are "
    "explicit nulls with a <field>Reason - never a silent zero.",
    RPC_PARAMS(
        ActorNameParamUtils::ActorNameParamReq(TEXT("string"),
            TEXT("Display label or name of a possessed Pawn or an AIController in the active PIE "
                 "world. The objectPath and actorPath aliases are also accepted."
                 ACTORNAME_COLLISION_STEER))
    ))
{
    if (!GEditor)
    {
        Ctx.SendError(ErrorCodes::ERR_EDITOR_NOT_AVAILABLE, TEXT("Editor not available"));
        return true;
    }

    FString ResolvedMode;
    UWorld* PieWorld = McpActorUtils::ResolveQueryWorld(TEXT("pie"), ResolvedMode);
    if (!PieWorld)
    {
        Ctx.SendError(ErrorCodes::ERR_NOT_IN_PIE,
            TEXT("ai.get_runtime_state reads a running AI, which only exists during play. "
                 "Start a session with editor.play, then re-issue."));
        return true;
    }

    // Resolution is scoped to the PIE world on purpose: the editor world holds the unpossessed
    // placement copies of the same actors, and answering about one of those would describe an
    // AI that is not running at all.
    AActor* Target = nullptr;
    FString RequestedName;
    if (!ActorNameParamUtils::RequireResolvedActor(Ctx, PieWorld, Target, &RequestedName))
    {
        return true;
    }

    AController* Controller = Cast<AController>(Target);
    if (!Controller)
    {
        if (APawn* Pawn = Cast<APawn>(Target))
        {
            Controller = Pawn->GetController();
        }
    }
    if (!Controller)
    {
        Ctx.SendError(ErrorCodes::ERR_NO_AI_CONTROLLER,
            FString::Printf(
                TEXT("'%s' is a %s with no controller in the PIE world. Name a possessed Pawn or ")
                TEXT("the AIController itself; an unpossessed Pawn runs no AI logic."),
                *RequestedName, *Target->GetClass()->GetName()));
        return true;
    }

    AAIController* AIController = Cast<AAIController>(Controller);
    UBrainComponent* Brain = AIController ? AIController->GetBrainComponent() : nullptr;
    if (!Brain)
    {
        Brain = Controller->FindComponentByClass<UBrainComponent>();
    }
    UPathFollowingComponent* PathFollowing = AIController ? AIController->GetPathFollowingComponent() : nullptr;
    if (!PathFollowing)
    {
        PathFollowing = Controller->FindComponentByClass<UPathFollowingComponent>();
    }
    UBlackboardComponent* Blackboard = AIController ? AIController->GetBlackboardComponent() : nullptr;
    if (!Blackboard && Brain)
    {
        Blackboard = Brain->GetBlackboardComponent();
    }
    if (!Blackboard)
    {
        Blackboard = Controller->FindComponentByClass<UBlackboardComponent>();
    }

    // A controller with neither a brain nor path following is not a running AI in any sense the
    // verb can report on, so it is refused rather than answered with three empty sections. A
    // controller that has one but not the other IS answered: an AIController driving a pawn with
    // a bare MoveTo and no Behavior Tree is precisely the "is it stuck?" case this verb serves.
    if (!Brain && !PathFollowing)
    {
        Ctx.SendError(ErrorCodes::ERR_NO_BRAIN_COMPONENT,
            FString::Printf(
                TEXT("Controller '%s' (%s) has neither a UBrainComponent nor a ")
                TEXT("UPathFollowingComponent, so it is running no AI logic to report on."),
                *Controller->GetName(), *Controller->GetClass()->GetName()));
        return true;
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("requestedName"), RequestedName);
    Result->SetStringField(TEXT("actorPath"), Target->GetPathName());
    Result->SetStringField(TEXT("actorClass"), Target->GetClass()->GetName());
    Result->SetStringField(TEXT("controllerPath"), Controller->GetPathName());
    Result->SetStringField(TEXT("controllerClass"), Controller->GetClass()->GetName());
    Result->SetBoolField(TEXT("isAIController"), AIController != nullptr);
    Result->SetStringField(TEXT("pieWorldPath"), PieWorld->GetPathName());
    if (const APawn* Pawn = Controller->GetPawn())
    {
        Result->SetStringField(TEXT("pawnPath"), Pawn->GetPathName());
    }
    else
    {
        AiRuntimeSetNull(Result, TEXT("pawnPath"), TEXT("pawnReason"),
            TEXT("The controller possesses no pawn."));
    }

    Result->SetObjectField(TEXT("brain"), AIRuntimeState::DescribeBrain(Brain));
    Result->SetObjectField(TEXT("pathFollowing"), AIRuntimeState::DescribePathFollowing(PathFollowing));
    Result->SetObjectField(TEXT("blackboard"), AIRuntimeState::DescribeBlackboard(Blackboard));

    Ctx.SendSuccess(Result);
    return true;
}
