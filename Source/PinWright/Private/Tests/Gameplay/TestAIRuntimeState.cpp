// Copyright (c) 2026 Alexander Penkin. MIT License.

// Tests for ai.get_runtime_state (Handlers/AI/AIRuntimeStateHandler.cpp).
//
// The verb itself needs a live PIE session with a possessed AI, which the unit suite cannot
// stand up: PIE is not started here, and starting it under -unattended walks dirty transient
// Blueprints left by sibling tests. So the coverage splits in two, and between them they reach
// everything except the populated (tree-running) branch:
//
//   1. The wire contract OUTSIDE PIE - the gate must refuse with NOT_IN_PIE before it resolves
//      anything, so a caller never gets an editor-world answer about an AI that is not running.
//   2. The pure state-describing seams (AIRuntimeState::Describe*), driven directly on real
//      UBehaviorTreeComponent / UPathFollowingComponent / UBlackboardComponent objects. Those
//      are the honesty half: a component with no path, no tree and no blackboard asset must
//      report explicit nulls with reasons, not zeros that read as measurements.
//
// UNTESTABLE HERE, and deliberately not faked: the populated BT branch of DescribeBrain
// (activeNode / activeNodePath / taskStatus). Reaching it needs UBehaviorTreeComponent::StartTree
// on a component owned by a possessed AIController inside a ticking world - the InstanceStack it
// reads is protected and cannot be seeded from outside. A constructed component always answers
// TreeHasBeenStarted()==false, which is the branch asserted below.
#include "Misc/AutomationTest.h"

#include "BehaviorTree/BehaviorTreeComponent.h"
#include "BehaviorTree/BlackboardComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Handlers/AI/AIRuntimeStateHandler.h"
#include "Navigation/PathFollowingComponent.h"
#include "Tests/TestSkipReporting.h"
#include "Tests/TestUtils.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

namespace
{
    // A field the handler wrote as an explicit JSON null. FJsonObject::HasField answers FALSE for
    // a null value, so the map is read directly: the assertion has to tell "written as null" from
    // "never written", which is the whole distinction this verb's contract rests on.
    bool AiRuntimeTestFieldIsNull(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field)
    {
        if (!Object.IsValid())
        {
            return false;
        }
        const TSharedPtr<FJsonValue>* Value = Object->Values.Find(Field);
        return Value != nullptr && Value->IsValid() && (*Value)->IsNull();
    }

    bool AiRuntimeTestHasNonEmptyString(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field)
    {
        FString Value;
        return Object.IsValid() && Object->TryGetStringField(Field, Value) && !Value.IsEmpty();
    }
}

// ---------------------------------------------------------------------------
// 1. The PIE gate.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAIGetRuntimeStateRefusesOutsidePieTest,
    "PinWright.ai.get_runtime_state.RefusesOutsidePie",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAIGetRuntimeStateRefusesOutsidePieTest::RunTest(const FString& Parameters)
{
    TestTrue(TEXT("ai.get_runtime_state is registered"),
        IsHandlerRegistered(TEXT("ai.get_runtime_state")));

    // The actor-identity slot must carry the alias set, or a caller feeding back the actorPath
    // that actor.spawn returned is refused at the wire before the body ever runs.
    TestTrue(TEXT("actorName is a declared parameter"),
        GetRegisteredParamSpec(TEXT("ai.get_runtime_state"), TEXT("actorName")) != nullptr);

    if (GEditor && GEditor->PlayWorld)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("pie-session-active"),
            TEXT("A PIE session is running on this host, so the not-in-PIE refusal cannot be observed."));
        return true;
    }

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("actorName"), TEXT("PinWrightRuntimeStateProbe"));

    FTestResponseCapture Capture;
    TestTrue(TEXT("handler invoked"),
        InvokeHandlerWithCapture(TEXT("ai.get_runtime_state"), Payload, Capture));
    TestFalse(TEXT("response is an error, not a fake success"), Capture.bSuccess);

    // The precise code matters: falling through to the editor world would answer ACTOR_NOT_FOUND
    // (or, worse, describe the unpossessed placement copy of a real AI) instead of saying that
    // there is no play session to read a running AI from.
    TestEqual(TEXT("error code is NOT_IN_PIE"), Capture.ErrorCode, FString(TEXT("NOT_IN_PIE")));
    TestTrue(TEXT("the message steers to editor.play"), Capture.Message.Contains(TEXT("editor.play")));
    return true;
}

// ---------------------------------------------------------------------------
// 2. The pure describe seams: absent things are stated, never zeroed.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAIGetRuntimeStateDescribesAbsentStateTest,
    "PinWright.ai.get_runtime_state.DescribesAbsentStateExplicitly",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAIGetRuntimeStateDescribesAbsentStateTest::RunTest(const FString& Parameters)
{
    // --- No component at all: each section says so, with a reason.
    TArray<TPair<FString, TSharedPtr<FJsonObject>>> NullSections;
    NullSections.Emplace(TEXT("brain"), AIRuntimeState::DescribeBrain(nullptr));
    NullSections.Emplace(TEXT("pathFollowing"), AIRuntimeState::DescribePathFollowing(nullptr));
    NullSections.Emplace(TEXT("blackboard"), AIRuntimeState::DescribeBlackboard(nullptr));
    for (const TPair<FString, TSharedPtr<FJsonObject>>& Section : NullSections)
    {
        bool bPresent = true;
        TestTrue(*FString::Printf(TEXT("%s section reports present"), *Section.Key),
            Section.Value.IsValid() && Section.Value->TryGetBoolField(TEXT("present"), bPresent));
        TestFalse(*FString::Printf(TEXT("%s section is absent for a null component"), *Section.Key),
            bPresent);
        TestTrue(*FString::Printf(TEXT("%s absence carries a reason"), *Section.Key),
            AiRuntimeTestHasNonEmptyString(Section.Value, TEXT("reason")));
    }

    // --- A real, unstarted Behavior Tree brain.
    TStrongObjectPtr<UBehaviorTreeComponent> BrainComp(
        NewObject<UBehaviorTreeComponent>(GetTransientPackage()));
    const TSharedPtr<FJsonObject> Brain = AIRuntimeState::DescribeBrain(BrainComp.Get());

    bool bBrainPresent = false;
    TestTrue(TEXT("brain section is present for a real component"),
        Brain.IsValid() && Brain->TryGetBoolField(TEXT("present"), bBrainPresent) && bBrainPresent);
    FString BrainClass;
    Brain->TryGetStringField(TEXT("componentClass"), BrainClass);
    TestEqual(TEXT("brain reports its component class"), BrainClass, FString(TEXT("BehaviorTreeComponent")));
    bool bIsBehaviorTree = false;
    TestTrue(TEXT("brain is recognised as a Behavior Tree brain"),
        Brain->TryGetBoolField(TEXT("isBehaviorTree"), bIsBehaviorTree) && bIsBehaviorTree);
    bool bRunning = true;
    TestTrue(TEXT("an unstarted brain reports isRunning false"),
        Brain->TryGetBoolField(TEXT("isRunning"), bRunning) && !bRunning);

    const TSharedPtr<FJsonObject>* BehaviorTree = nullptr;
    if (TestTrue(TEXT("brain carries a behaviorTree section"),
            Brain->TryGetObjectField(TEXT("behaviorTree"), BehaviorTree) && BehaviorTree))
    {
        bool bTreePresent = true;
        TestTrue(TEXT("an unstarted tree is reported absent, not empty"),
            (*BehaviorTree)->TryGetBoolField(TEXT("present"), bTreePresent) && !bTreePresent);
        TestTrue(TEXT("the absent tree carries a reason"),
            AiRuntimeTestHasNonEmptyString(*BehaviorTree, TEXT("reason")));
    }

    // --- A real, idle path-following component. This is the counterfactual half of the ticket:
    // reporting the stale CurrentDestination (or a zero vector) as the goal is the false answer.
    TStrongObjectPtr<UPathFollowingComponent> PathComp(
        NewObject<UPathFollowingComponent>(GetTransientPackage()));
    const TSharedPtr<FJsonObject> PathFollowing = AIRuntimeState::DescribePathFollowing(PathComp.Get());

    bool bPathPresent = false;
    TestTrue(TEXT("pathFollowing section is present for a real component"),
        PathFollowing.IsValid()
            && PathFollowing->TryGetBoolField(TEXT("present"), bPathPresent) && bPathPresent);
    // The name comes from the engine's own GetStatusDesc (reflection over EPathFollowingStatus),
    // so the assertion is on the token rather than on how UEnum spells the scope prefix.
    FString StatusDesc;
    PathFollowing->TryGetStringField(TEXT("status"), StatusDesc);
    TestTrue(TEXT("an idle component names the Idle status"), StatusDesc.Contains(TEXT("Idle")));
    double StatusValue = -1.0;
    TestTrue(TEXT("statusValue carries the raw EPathFollowingStatus"),
        PathFollowing->TryGetNumberField(TEXT("statusValue"), StatusValue));
    TestEqual(TEXT("EPathFollowingStatus::Idle is 0"), static_cast<int32>(StatusValue), 0);
    bool bHasValidPath = true;
    TestTrue(TEXT("an idle component reports hasValidPath false"),
        PathFollowing->TryGetBoolField(TEXT("hasValidPath"), bHasValidPath) && !bHasValidPath);

    TestTrue(TEXT("currentTargetLocation is an explicit null with no path"),
        AiRuntimeTestFieldIsNull(PathFollowing, TEXT("currentTargetLocation")));
    TestTrue(TEXT("the null target location carries a reason"),
        AiRuntimeTestHasNonEmptyString(PathFollowing, TEXT("currentTargetLocationReason")));
    TestTrue(TEXT("pathEndLocation is an explicit null with no path"),
        AiRuntimeTestFieldIsNull(PathFollowing, TEXT("pathEndLocation")));
    TestTrue(TEXT("lastMoveResult is an explicit null"),
        AiRuntimeTestFieldIsNull(PathFollowing, TEXT("lastMoveResult")));
    TestTrue(TEXT("lastMoveResult explains why the engine does not expose it"),
        AiRuntimeTestHasNonEmptyString(PathFollowing, TEXT("lastMoveResultReason")));

    // --- A real Blackboard component with no asset bound. GetNumKeys() answers 0 here exactly as
    // it would for a bound-but-empty board, so the empty keys array has to be qualified.
    TStrongObjectPtr<UBlackboardComponent> BlackboardComp(
        NewObject<UBlackboardComponent>(GetTransientPackage()));
    const TSharedPtr<FJsonObject> Blackboard = AIRuntimeState::DescribeBlackboard(BlackboardComp.Get());

    bool bBlackboardPresent = false;
    TestTrue(TEXT("blackboard section is present for a real component"),
        Blackboard.IsValid()
            && Blackboard->TryGetBoolField(TEXT("present"), bBlackboardPresent) && bBlackboardPresent);
    TestTrue(TEXT("assetPath is an explicit null when no UBlackboardData is bound"),
        AiRuntimeTestFieldIsNull(Blackboard, TEXT("assetPath")));
    TestTrue(TEXT("the unbound asset carries a reason"),
        AiRuntimeTestHasNonEmptyString(Blackboard, TEXT("assetReason")));
    const TArray<TSharedPtr<FJsonValue>>* Keys = nullptr;
    TestTrue(TEXT("blackboard emits a keys array"),
        Blackboard->TryGetArrayField(TEXT("keys"), Keys) && Keys);
    if (Keys)
    {
        TestEqual(TEXT("an unbound blackboard dumps no keys"), Keys->Num(), 0);
    }
    return true;
}
