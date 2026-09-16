// Copyright (c) 2026 Alexander Penkin. MIT License.

// Unit tests for Animation domain handlers.
// Covers all 6 handler files: AnimationHandler, AnimationAuthoringHandler,
// MorphTargetHandler, PhysicsAssetHandler, SkeletalMeshHandler, SkeletonHandler.
// Most tests invoke registered handlers via the auto-registration table and
// assert that the handler was found and did not crash. Removed-handler tests
// assert that retired RPC names stay absent.
#include "Misc/AutomationTest.h"
#include "Misc/EngineVersionComparison.h"
#include "Animation/AnimData/CurveIdentifier.h"
#include "Animation/AnimData/IAnimationDataController.h"
#include "Animation/AnimComposite.h"
#include "Animation/AnimCompositeBase.h"
#include "Animation/AnimMontage.h"
#include "Animation/AnimNotifies/AnimNotify.h"
#include "Animation/AnimNotifies/AnimNotifyState_DisableRootMotion.h"
#include "Animation/AnimSequence.h"
#include "Animation/BlendSpace.h"
#include "Animation/BlendSpace1D.h"
#include "Animation/Skeleton.h"
#include "Animation/AnimTypes.h"
#include "Handlers/Animation/AnimationAuthoringHelpers.h"
#include "Handlers/Animation/AnimSequenceCreate.h"
#include "Curves/RichCurve.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ErrorCodes.h"
#include "Handlers/HandlerRegistration.h"
#include "Handlers/Animation/SkinWeightTransferUtils.h"
#include "Handlers/ParamSpec.h"
#include "Misc/FrameNumber.h"
#include "Misc/FrameRate.h"
#include "Misc/Guid.h"
#include "ReferenceSkeleton.h"
#include "Engine/SkeletalMesh.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "PhysicsEngine/BodySetup.h"
#if __has_include("PhysicsEngine/SkeletalBodySetup.h")
#include "PhysicsEngine/SkeletalBodySetup.h"
#endif
#include "Tests/Gameplay/TestAnimationNotifyStatePropertyFixtures.h"
#include "Tests/Gameplay/TestAnimationFixtures.h"
#include "Tests/TestUtils.h"
#include "Utils/BlueprintGraphSnapshot.h"
#include "Utils/LogUtils.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"


// IK Rig / IK Retargeter authoring family regression coverage (ticket
// F-ik-rig-retargeter-family-not-compiled). These headers only resolve when the
// IKRig/IKRigEditor module deps are present in Build.cs; the same __has_include guards the
// production handler uses gate the test so it round-trips the real engine API rather than a
// copy. If the Build.cs deps (or the MCP_HAS_IKRIG "Rig/IKRigDefinition.h" guard fix) are
// reverted, MCP_TEST_HAS_IKRIG flips to 0 and the family-enabled assertions below fail.
#if __has_include("Rig/IKRigDefinition.h") && __has_include("RigEditor/IKRigController.h")
#include "Rig/IKRigDefinition.h"
#include "RigEditor/IKRigController.h"
#define MCP_TEST_HAS_IKRIG 1
#else
#define MCP_TEST_HAS_IKRIG 0
#endif

// State-machine creation regression coverage (ticket B-create-state-machine-exec-failed).
// The animation.create_state_machine handler now builds the graph directly via
// AnimGraphConstructionUtils instead of dispatching dead console verbs through
// GEditor->Exec. These headers gate the success-path assertions on the same
// AnimGraph state-machine module the production handler requires; if that module
// is unavailable the test asserts only that the handler is registered. The fresh
// transient AnimBlueprint is built through the shared AGIRTestFixtures factory so
// this test does not re-derive the AnimBlueprintFactory boilerplate.
#include "Animation/AnimBlueprint.h"
#include "Handlers/Animation/AnimGraphConstructionUtils.h"
#include "Handlers/Animation/AnimationHandlerTestHooks.h"
#include "EdGraph/EdGraph.h"
#include "Tests/Assets/TestAGIRFixtures.h"
#if __has_include("AnimGraphNode_StateMachine.h") && __has_include("AnimStateNode.h") && \
    __has_include("AnimStateTransitionNode.h") && __has_include("AnimationStateMachineGraph.h")
#include "AnimGraphNode_StateMachine.h"
#include "AnimStateNode.h"
#include "AnimStateNodeBase.h"
#include "AnimStateTransitionNode.h"
#include "AnimationStateMachineGraph.h"
#define MCP_TEST_HAS_STATE_MACHINE_GRAPH 1
#else
#define MCP_TEST_HAS_STATE_MACHINE_GRAPH 0
#endif

// Pose-library creation regression coverage (ticket B-create-pose-library-noop-fake-success).
// Gated on the same "Animation/PoseAsset.h" header the production handler tests with
// MCP_HAS_POSEASSET, so the round-trip exercises the real engine type rather than a copy.
#if __has_include("Animation/PoseAsset.h")
#include "Animation/PoseAsset.h"
#define MCP_TEST_HAS_POSEASSET 1
#else
#define MCP_TEST_HAS_POSEASSET 0
#endif

static UAnimSequence* NewLoadedTransientAnimSequence(
    const TCHAR* Prefix,
    FString& OutObjectPath,
    FFrameRate FrameRate = FFrameRate(30, 1),
    USkeleton* Skeleton = nullptr)
{
    const FString AssetName = FString::Printf(TEXT("%s_%s"), Prefix, *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    const FString PackageName = FString::Printf(TEXT("/Game/PinWrightTests/%s"), *AssetName);
    UPackage* Package = CreatePackage(*PackageName);
    UAnimSequence* Sequence = NewObject<UAnimSequence>(Package, *AssetName, RF_Public | RF_Standalone | RF_Transient);
    if (!Sequence)
    {
        return nullptr;
    }

    if (Skeleton)
    {
        Sequence->SetSkeleton(Skeleton);
    }

    IAnimationDataController& Controller = Sequence->GetController();
    // Mirror UAnimSequenceFactory init so authored frame-rate data exists on transient test assets too.
    const FFrameNumber NumberOfFrames(2);
    Controller.InitializeModel();
    Controller.SetFrameRate(FrameRate);
    Controller.SetNumberOfFrames(NumberOfFrames);
    // On UE 5.3 and 5.4 the AnimationData plugin's experimental Sequencer-based data model backs
    // every UAnimSequence with an FK Control Rig (UFKControlRig). Populating a bone track for a
    // skeleton with this model spins up an async compressed-data DDC build that evaluates the FK
    // rig on a background "Foreground Worker" thread; with a minimal skeleton that evaluation
    // asserts `Array index out of bounds` inside ControlRig/RigVM and kills the editor. It was
    // originally assumed 5.4 used the stable UAnimDataModel path, but the 5.4 host reproduces the
    // identical fatal ControlRig array-OOB assert, so the skip extends through 5.4. No test relies
    // on the helper's root bone keys (callers assert on segments, length, or their own curves), so
    // skip the bone track on 5.3/5.4. 5.5+ uses the stable path and keeps the bone keys.
#if !UE_VERSION_OLDER_THAN(5, 5, 0)
    if (Skeleton && Skeleton->GetReferenceSkeleton().GetRawBoneNum() > 0)
    {
        const FReferenceSkeleton& RefSkeleton = Skeleton->GetReferenceSkeleton();
        const FName RootBoneName = RefSkeleton.GetBoneName(0);
        const FTransform& RootPose = RefSkeleton.GetRawRefBonePose()[0];
        const int32 NumKeys = NumberOfFrames.Value + 1;
        TArray<FVector3f> PositionalKeys;
        TArray<FQuat4f> RotationalKeys;
        TArray<FVector3f> ScalingKeys;
        PositionalKeys.Init(FVector3f(RootPose.GetTranslation()), NumKeys);
        RotationalKeys.Init(FQuat4f(RootPose.GetRotation()), NumKeys);
        ScalingKeys.Init(FVector3f(RootPose.GetScale3D()), NumKeys);
        Controller.AddBoneCurve(RootBoneName);
        Controller.SetBoneTrackKeys(RootBoneName, PositionalKeys, RotationalKeys, ScalingKeys);
    }
#endif
    Controller.NotifyPopulated();
    Sequence->AddToRoot();

    OutObjectPath = FString::Printf(TEXT("%s.%s"), *PackageName, *AssetName);
    return Sequence;
}

static USkeleton* NewLoadedTransientSkeleton(const TCHAR* Prefix, FString& OutObjectPath)
{
    const FString AssetName = FString::Printf(TEXT("%s_%s"), Prefix, *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    const FString PackageName = FString::Printf(TEXT("/Game/PinWrightTests/%s"), *AssetName);
    UPackage* Package = CreatePackage(*PackageName);
    USkeleton* Skeleton = NewObject<USkeleton>(Package, *AssetName, RF_Public | RF_Standalone | RF_Transient);
    if (!Skeleton)
    {
        return nullptr;
    }

    FReferenceSkeletonModifier Modifier(Skeleton);
    FMeshBoneInfo RootBone;
    RootBone.Name = FName(TEXT("root"));
    RootBone.ParentIndex = INDEX_NONE;
    RootBone.ExportName = TEXT("root");
    Modifier.Add(RootBone, FTransform::Identity, true);

    Skeleton->AddToRoot();
    OutObjectPath = FString::Printf(TEXT("%s.%s"), *PackageName, *AssetName);
    return Skeleton;
}

// OutDifference names the first divergence found, so a failure says which graph or node the
// rollback left behind instead of only that the topologies differ.
static bool BlueprintGraphSnapshotsMatch(
    const BlueprintGraphSnapshot::FBlueprintGraphSnapshot& Expected,
    const BlueprintGraphSnapshot::FBlueprintGraphSnapshot& Actual,
    FString& OutDifference)
{
    OutDifference.Reset();
    if (Expected.Blueprint != Actual.Blueprint)
    {
        OutDifference = TEXT("snapshots describe different Blueprints");
        return false;
    }

    if (Expected.NodesByGraph.Num() != Actual.NodesByGraph.Num())
    {
        TArray<FString> AddedGraphs;
        for (const TPair<TWeakObjectPtr<UEdGraph>, TSet<TWeakObjectPtr<UEdGraphNode>>>& Pair :
             Actual.NodesByGraph)
        {
            if (!Expected.NodesByGraph.Contains(Pair.Key))
            {
                const UEdGraph* Graph = Pair.Key.Get();
                AddedGraphs.Add(Graph
                    ? FString::Printf(TEXT("%s (%s, %d node(s))"), *Graph->GetName(),
                        *Graph->GetClass()->GetName(), Pair.Value.Num())
                    : FString(TEXT("<stale graph>")));
            }
        }
        OutDifference = FString::Printf(
            TEXT("graph count %d -> %d; graphs not in the pre-image: [%s]"),
            Expected.NodesByGraph.Num(), Actual.NodesByGraph.Num(),
            *FString::Join(AddedGraphs, TEXT(", ")));
        return false;
    }

    for (const TPair<TWeakObjectPtr<UEdGraph>, TSet<TWeakObjectPtr<UEdGraphNode>>>& Pair :
         Expected.NodesByGraph)
    {
        const UEdGraph* Graph = Pair.Key.Get();
        const FString GraphName = Graph ? Graph->GetName() : FString(TEXT("<stale graph>"));
        const TSet<TWeakObjectPtr<UEdGraphNode>>* ActualNodes =
            Actual.NodesByGraph.Find(Pair.Key);
        if (!ActualNodes)
        {
            OutDifference = FString::Printf(
                TEXT("graph '%s' is missing after rollback"), *GraphName);
            return false;
        }
        if (ActualNodes->Num() != Pair.Value.Num())
        {
            OutDifference = FString::Printf(
                TEXT("graph '%s' node count %d -> %d"),
                *GraphName, Pair.Value.Num(), ActualNodes->Num());
            return false;
        }

        for (const TWeakObjectPtr<UEdGraphNode>& Node : Pair.Value)
        {
            if (!ActualNodes->Contains(Node))
            {
                const UEdGraphNode* NodePtr = Node.Get();
                OutDifference = FString::Printf(
                    TEXT("graph '%s' no longer holds node '%s'"), *GraphName,
                    NodePtr ? *NodePtr->GetName() : TEXT("<stale node>"));
                return false;
            }
        }
    }

    return true;
}

// Sibling of NewLoadedTransientAnimSequence/NewLoadedTransientSkeleton for blend-space tests:
// same GUID-suffixed /Game/PinWrightTests package + RF_Transient construction + AddToRoot +
// OutObjectPath convention. Returns the rooted (UObject::IsAsset()==false) UBlendSpace1D so the
// caller configures the blend-space-specific axis/sample inline.
static UBlendSpace1D* NewLoadedTransientBlendSpace1D(const TCHAR* Prefix, FString& OutObjectPath)
{
    const FString AssetName = FString::Printf(TEXT("%s_%s"), Prefix, *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    const FString PackageName = FString::Printf(TEXT("/Game/PinWrightTests/%s"), *AssetName);
    UPackage* Package = CreatePackage(*PackageName);
    UBlendSpace1D* BlendSpace = NewObject<UBlendSpace1D>(Package, *AssetName, RF_Public | RF_Standalone | RF_Transient);
    if (!BlendSpace)
    {
        return nullptr;
    }

    BlendSpace->AddToRoot();
    OutObjectPath = FString::Printf(TEXT("%s.%s"), *PackageName, *AssetName);
    return BlendSpace;
}

// Sibling of the NewLoadedTransient* factories for montage tests: same GUID-suffixed
// /Game/PinWrightTests package + RF_Transient construction + AddToRoot + OutObjectPath
// convention. Returns the rooted UAnimMontage so the caller configures the montage-specific
// composite sections / slots inline.
static UAnimMontage* NewLoadedTransientAnimMontage(const TCHAR* Prefix, FString& OutObjectPath)
{
    const FString AssetName = FString::Printf(TEXT("%s_%s"), Prefix, *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    const FString PackageName = FString::Printf(TEXT("/Game/PinWrightTests/%s"), *AssetName);
    UPackage* Package = CreatePackage(*PackageName);
    UAnimMontage* Montage = NewObject<UAnimMontage>(Package, *AssetName, RF_Public | RF_Standalone | RF_Transient);
    if (!Montage)
    {
        return nullptr;
    }

    Montage->AddToRoot();
    OutObjectPath = FString::Printf(TEXT("%s.%s"), *PackageName, *AssetName);
    return Montage;
}

// ============================================================================
// AnimationHandler.cpp — 10 handlers
// ============================================================================

// Regression for B-create-state-machine-exec-failed: animation.create_state_machine
// used to build legacy console-verb strings (AddAnimStateMachine/AddAnimState/...) and
// run them through GEditor->Exec, which has no consumer for those verbs, so the method
// returned [COMMAND_FAILED] on command #1 for ANY input and created nothing. The fix
// drives the graph directly via AnimGraphConstructionUtils. This test creates a fresh
// transient AnimBlueprint, invokes the real handler with a two-state/one-transition
// payload, asserts it SUCCEEDS, and confirms the state machine node + both states + the
// transition actually exist in the AnimGraph. Reverting the fix restores the dead Exec
// dispatch: the handler errors with COMMAND_FAILED (Capture.bSuccess false) and no
// UAnimGraphNode_StateMachine is created, so these assertions fail.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAnimationCreateStateMachineBuildsGraphTest,
    "PinWright.animation.create_state_machine.BuildsRealGraph",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FAnimationCreateStateMachineBuildsGraphTest::RunTest(const FString& Parameters)
{
#if MCP_TEST_HAS_STATE_MACHINE_GRAPH
    FString SkeletonPath;
    USkeleton* Skeleton = NewLoadedTransientSkeleton(TEXT("SK_CreateSM"), SkeletonPath);
    TestNotNull(TEXT("transient skeleton created"), Skeleton);
    if (!Skeleton)
    {
        return false;
    }

    const FString AssetName = FString::Printf(
        TEXT("ABP_CreateSM_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    const FString PackageName = FString::Printf(
        TEXT("/Game/PinWrightTests/%s"), *AssetName);
    const FString ObjectPath = FString::Printf(TEXT("%s.%s"), *PackageName, *AssetName);

    // Build the fresh transient AnimBlueprint via the shared AGIR fixture (which
    // routes through IrTest::CreateFactoryAssetAtPath) rather than re-deriving the
    // AnimBlueprintFactory wiring here.
    UAnimBlueprint* AnimBP = AGIRTestFixtures::CreateFreshAnimBlueprint(PackageName, Skeleton);
    TestNotNull(TEXT("transient AnimBlueprint created"), AnimBP);
    if (!AnimBP)
    {
        Skeleton->RemoveFromRoot();
        return false;
    }

    // Build the payload that previously died on AddAnimStateMachine: a named machine,
    // two states (one flagged isEntry), and one transition between them.
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("blueprintPath"), ObjectPath);
    Payload->SetStringField(TEXT("machineName"), TEXT("Locomotion"));

    TArray<TSharedPtr<FJsonValue>> States;
    {
        TSharedPtr<FJsonObject> Idle = MakeShared<FJsonObject>();
        Idle->SetStringField(TEXT("name"), TEXT("Idle"));
        Idle->SetBoolField(TEXT("isEntry"), true);
        States.Add(MakeShared<FJsonValueObject>(Idle));

        TSharedPtr<FJsonObject> Walk = MakeShared<FJsonObject>();
        Walk->SetStringField(TEXT("name"), TEXT("WalkRun"));
        States.Add(MakeShared<FJsonValueObject>(Walk));
    }
    Payload->SetArrayField(TEXT("states"), States);

    TArray<TSharedPtr<FJsonValue>> Transitions;
    {
        TSharedPtr<FJsonObject> Trans = MakeShared<FJsonObject>();
        Trans->SetStringField(TEXT("sourceState"), TEXT("Idle"));
        Trans->SetStringField(TEXT("targetState"), TEXT("WalkRun"));
        Transitions.Add(MakeShared<FJsonValueObject>(Trans));
    }
    Payload->SetArrayField(TEXT("transitions"), Transitions);

    FTestResponseCapture Capture;
    const bool bFound = InvokeHandlerWithCapture(
        TEXT("animation.create_state_machine"), Payload, Capture);
    TestTrue(TEXT("animation.create_state_machine handler found"), bFound);
    // Core regression assertion: pre-fix this was COMMAND_FAILED (Exec found no consumer).
    TestTrue(TEXT("create_state_machine succeeds (was COMMAND_FAILED before fix)"),
        Capture.bWasCalled && Capture.bSuccess);

    // Confirm the handler manipulated the real graph: the state machine node, both
    // states, and the transition must now exist.
    UEdGraph* AnimGraph = AnimGraphConstructionUtils::GetAnimGraphFromBlueprint(AnimBP);
    TestNotNull(TEXT("AnimGraph present"), AnimGraph);
    if (AnimGraph)
    {
        UAnimGraphNode_StateMachine* SMNode =
            AnimGraphConstructionUtils::FindStateMachineNode(AnimGraph, TEXT("Locomotion"));
        TestNotNull(TEXT("state machine node 'Locomotion' created"), SMNode);
        if (SMNode && SMNode->EditorStateMachineGraph)
        {
            UAnimationStateMachineGraph* SMGraph =
                Cast<UAnimationStateMachineGraph>(SMNode->EditorStateMachineGraph);
            TestNotNull(TEXT("state machine graph castable"), SMGraph);
            if (SMGraph)
            {
                TestNotNull(TEXT("state 'Idle' created"),
                    AnimGraphConstructionUtils::FindStateNode(SMGraph, TEXT("Idle")));
                TestNotNull(TEXT("state 'WalkRun' created"),
                    AnimGraphConstructionUtils::FindStateNode(SMGraph, TEXT("WalkRun")));

                int32 TransitionNodeCount = 0;
                for (UEdGraphNode* Node : SMGraph->Nodes)
                {
                    if (Cast<UAnimStateTransitionNode>(Node))
                    {
                        ++TransitionNodeCount;
                    }
                }
                TestEqual(TEXT("exactly one transition node created"), TransitionNodeCount, 1);
            }
        }
    }

    CleanupTestAsset(PackageName);
    Skeleton->RemoveFromRoot();
    return true;
#else
    // AnimGraph state-machine module headers absent in this build — assert only that
    // the handler is registered so the test still links and passes.
    TestTrue(TEXT("animation.create_state_machine handler registered"),
        IsHandlerRegistered(TEXT("animation.create_state_machine")));
    return true;
#endif
}

// Regression for B-animation-create-state-machine-failure-not-atomic: the old
// handler created the machine and valid first state before a later failure. The
// development-only seam below forces the second state factory call to fail, after
// those mutations; the authored graph must return to its pre-request snapshot.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAnimationCreateStateMachineRollbackOnMidwayFailureTest,
    "PinWright.animation.create_state_machine.RollbackOnMidwayFailure",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FAnimationCreateStateMachineRollbackOnMidwayFailureTest::RunTest(const FString& Parameters)
{
#if MCP_TEST_HAS_STATE_MACHINE_GRAPH
    FString SkeletonPath;
    USkeleton* Skeleton = NewLoadedTransientSkeleton(TEXT("SK_CreateSMAtomic"), SkeletonPath);
    TestNotNull(TEXT("transient skeleton created"), Skeleton);
    if (!Skeleton)
    {
        return false;
    }

    const FString AssetName = FString::Printf(
        TEXT("ABP_CreateSMAtomic_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    const FString PackageName = FString::Printf(
        TEXT("/Game/PinWrightTests/%s"), *AssetName);
    const FString ObjectPath = FString::Printf(TEXT("%s.%s"), *PackageName, *AssetName);

    UAnimBlueprint* AnimBP = AGIRTestFixtures::CreateFreshAnimBlueprint(PackageName, Skeleton);
    TestNotNull(TEXT("transient AnimBlueprint created"), AnimBP);
    if (!AnimBP)
    {
        Skeleton->RemoveFromRoot();
        return false;
    }

    UPackage* Package = AnimBP->GetOutermost();
    Package->SetDirtyFlag(false);
    const bool bPackageWasDirty = Package->IsDirty();
    TestFalse(TEXT("package starts clean"), bPackageWasDirty);
    const BlueprintGraphSnapshot::FBlueprintGraphSnapshot Before =
        BlueprintGraphSnapshot::Capture(AnimBP);

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("blueprintPath"), ObjectPath);
    Payload->SetStringField(TEXT("machineName"), TEXT("AtomicSM"));

    TSharedPtr<FJsonObject> IdleState = MakeShared<FJsonObject>();
    IdleState->SetStringField(TEXT("name"), TEXT("Idle"));
    IdleState->SetBoolField(TEXT("isEntry"), true);
    TSharedPtr<FJsonObject> RunState = MakeShared<FJsonObject>();
    RunState->SetStringField(TEXT("name"), TEXT("Run"));
    Payload->SetArrayField(
        TEXT("states"),
        {MakeShared<FJsonValueObject>(IdleState),
         MakeShared<FJsonValueObject>(RunState)});

    FTestResponseCapture Capture;
    {
        AnimationHandlerTestHooks::FScopedCreateStateFailure FailSecondState(1);
        TestTrue(TEXT("second-state failure seam is armed"),
            AnimationHandlerTestHooks::ShouldFailCreateState(1));
        TestTrue(TEXT("animation.create_state_machine handler found"),
            InvokeHandlerWithCapture(TEXT("animation.create_state_machine"), Payload, Capture));
    }
    TestTrue(TEXT("handler responded with failure"), Capture.bWasCalled && !Capture.bSuccess);
    TestEqual(TEXT("injected second-state factory failure reports STATE_CREATE_FAILED"),
        Capture.ErrorCode, FString(TEXT("STATE_CREATE_FAILED")));

    const BlueprintGraphSnapshot::FBlueprintGraphSnapshot After =
        BlueprintGraphSnapshot::Capture(AnimBP);
    FString TopologyDifference;
    const bool bTopologyUnchanged =
        BlueprintGraphSnapshotsMatch(Before, After, TopologyDifference);
    TestTrue(FString::Printf(
            TEXT("AnimBP graph topology is unchanged after post-mutation failure (%s)"),
            *TopologyDifference),
        bTopologyUnchanged);
    TestEqual(TEXT("failed request preserves package dirty flag"),
        Package->IsDirty(), bPackageWasDirty);

    CleanupTestAsset(PackageName);
    Skeleton->RemoveFromRoot();
    return true;
#else
    TestTrue(TEXT("animation.create_state_machine handler registered"),
        IsHandlerRegistered(TEXT("animation.create_state_machine")));
    return true;
#endif
}

// ============================================================================
// AnimationAuthoringHandler.cpp — 34 handlers (representative coverage)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAuthoringListCurvesReturnsDumpShapeTest,
    "PinWright.animation.authoring.list_curves.ReturnsDumpShape",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FAuthoringListCurvesReturnsDumpShapeTest::RunTest(const FString& Parameters)
{
    const FHandlerRegistration* Registration = nullptr;
    for (const FHandlerRegistration& Reg : FAutoRegisterHandler::GetPendingRegistrations())
    {
        if (Reg.MethodName == TEXT("animation.authoring.list_curves"))
        {
            Registration = &Reg;
            break;
        }
    }

    TestNotNull(TEXT("animation.authoring.list_curves handler registered"), Registration);
    if (!Registration)
    {
        return true;
    }

    TestEqual(TEXT("animation.authoring.list_curves category"), Registration->Category, FString(TEXT("animation.authoring")));
    bool bHasRequiredAssetPath = false;
    for (const FParamSpec& Param : Registration->Params)
    {
        if (Param.Name == TEXT("assetPath") && Param.Type == TEXT("path") && Param.bRequired)
        {
            bHasRequiredAssetPath = true;
            break;
        }
    }
    TestTrue(TEXT("assetPath is required"), bHasRequiredAssetPath);

    FString SkeletonPath;
    USkeleton* Skeleton = NewLoadedTransientSkeleton(TEXT("SK_ListCurves"), SkeletonPath);
    TestNotNull(TEXT("transient skeleton created"), Skeleton);
    if (!Skeleton)
    {
        return true;
    }

    FString ObjectPath;
    UAnimSequence* Sequence = NewLoadedTransientAnimSequence(
        TEXT("AS_ListCurves"), ObjectPath, FFrameRate(30, 1), Skeleton);
    TestNotNull(TEXT("transient sequence created"), Sequence);
    if (!Sequence)
    {
        Skeleton->RemoveFromRoot();
        return true;
    }

    IAnimationDataController& Controller = Sequence->GetController();
    // Bracket the post-populate edits so the async compressed-data DDC build launches once, on a
    // complete model, at CloseBracket. Unbracketed, the first edit kicks a background build that
    // races the remaining edits on the game thread — observed on UE 5.5 as a fatal
    // "Array index out of bounds: 0 into an array of size 0" on Foreground Worker #0 when the
    // build snapshotted a just-added, still-keyless curve.
    Controller.OpenBracket(FText::FromString(TEXT("PinWright ListCurves Test")), /*bShouldTransact=*/false);
    Controller.SetFrameRate(FFrameRate(30, 1));
    Controller.SetNumberOfFrames(FFrameNumber(2));

    const FAnimationCurveIdentifier FloatCurveId(FName(TEXT("FloatCurve")), ERawCurveTrackTypes::RCT_Float);
    Controller.AddCurve(FloatCurveId, AACF_DefaultCurve);
    Controller.SetCurveKey(FloatCurveId, FRichCurveKey(0.0f, 1.0f));
    Controller.SetCurveKey(FloatCurveId, FRichCurveKey(1.0f / 30.0f, 2.0f));

    const FAnimationCurveIdentifier TransformCurveId(FName(TEXT("TransformCurve")), ERawCurveTrackTypes::RCT_Transform);
    Controller.AddCurve(TransformCurveId, AACF_DriveTrack | AACF_Editable);
    Controller.SetTransformCurveKey(TransformCurveId, 0.0f, FTransform::Identity);
    Controller.CloseBracket(/*bShouldTransact=*/false);

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), ObjectPath);

    FTestResponseCapture Capture;
    TestTrue(TEXT("animation.authoring.list_curves handler found"),
        InvokeHandlerWithCapture(TEXT("animation.authoring.list_curves"), Payload, Capture));
    TestTrue(TEXT("list_curves succeeds"), Capture.bSuccess);

    const TArray<TSharedPtr<FJsonValue>>* Curves = nullptr;
    if (Capture.Result.IsValid())
    {
        TestTrue(TEXT("curves array present"), Capture.Result->TryGetArrayField(TEXT("curves"), Curves));
    }
    if (!Curves)
    {
        Sequence->RemoveFromRoot();
        Skeleton->RemoveFromRoot();
        return true;
    }
    TestEqual(TEXT("curve count"), Curves->Num(), 2);

    int32 FloatKeyCount = INDEX_NONE;
    int32 TransformKeyCount = INDEX_NONE;
    for (const TSharedPtr<FJsonValue>& CurveValue : *Curves)
    {
        const TSharedPtr<FJsonObject> CurveObject = CurveValue.IsValid() ? CurveValue->AsObject() : nullptr;
        if (!CurveObject.IsValid())
        {
            continue;
        }

        const FString CurveName = CurveObject->GetStringField(TEXT("name"));
        const FString CurveType = CurveObject->GetStringField(TEXT("type"));
        const int32 KeyCount = static_cast<int32>(CurveObject->GetNumberField(TEXT("keyCount")));
        if (CurveName == TEXT("FloatCurve") && CurveType == TEXT("Float"))
        {
            FloatKeyCount = KeyCount;
        }
        else if (CurveName == TEXT("TransformCurve") && CurveType == TEXT("Transform"))
        {
            TransformKeyCount = KeyCount;
        }
    }

    TestEqual(TEXT("Float curve keyCount matches dump shape"), FloatKeyCount, 2);
    TestEqual(TEXT("Transform curve keyCount totals all transform channels"), TransformKeyCount, 9);
    Sequence->RemoveFromRoot();
    Skeleton->RemoveFromRoot();
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAuthoringListNotifiesReturnsSortedDumpParityShapeTest,
    "PinWright.animation.authoring.list_notifies.ReturnsSortedDumpParityShape",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FAuthoringListNotifiesReturnsSortedDumpParityShapeTest::RunTest(const FString& Parameters)
{
    const FHandlerRegistration* Registration = nullptr;
    for (const FHandlerRegistration& Reg : FAutoRegisterHandler::GetPendingRegistrations())
    {
        if (Reg.MethodName == TEXT("animation.authoring.list_notifies"))
        {
            Registration = &Reg;
            break;
        }
    }

    TestNotNull(TEXT("animation.authoring.list_notifies handler registered"), Registration);
    if (!Registration)
    {
        return true;
    }

    TestEqual(TEXT("animation.authoring.list_notifies category"), Registration->Category, FString(TEXT("animation.authoring")));
    bool bHasRequiredAssetPath = false;
    for (const FParamSpec& Param : Registration->Params)
    {
        if (Param.Name == TEXT("assetPath") && Param.Type == TEXT("path") && Param.bRequired)
        {
            bHasRequiredAssetPath = true;
            break;
        }
    }
    TestTrue(TEXT("assetPath is required"), bHasRequiredAssetPath);

    PinWrightAnimationTestFixtures::FRegisteredAnimationFixture Fixture;
    TestTrue(TEXT("registered animation fixture created"),
        Fixture.Create(TEXT("AS_ListNotifies")));
    UAnimSequence* Sequence = Fixture.Sequence;
    const FString ObjectPath = Fixture.SequenceObjectPath;
    TestNotNull(TEXT("registered sequence created"), Sequence);
    if (!Sequence)
    {
        return true;
    }

    auto AddNotify = [Sequence](const TCHAR* NotifyName, float Time, float Duration)
    {
        FAnimNotifyEvent Event;
        Event.NotifyName = FName(NotifyName);
        Event.NotifyStateClass = NewObject<UAnimNotifyState_DisableRootMotion>(Sequence);
        Event.SetTime(Time);
        Event.SetDuration(Duration);
        Sequence->Notifies.Add(Event);
    };

    AddNotify(TEXT("Zed"), 0.25f, 0.10f);
    AddNotify(TEXT("Bravo"), 0.10f, 0.05f);
    AddNotify(TEXT("Alpha"), 0.25f, 0.15f);

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), ObjectPath);

    FTestResponseCapture Capture;
    TestTrue(TEXT("animation.authoring.list_notifies handler found"),
        InvokeHandlerWithCapture(TEXT("animation.authoring.list_notifies"), Payload, Capture));
    TestTrue(TEXT("list_notifies succeeds"), Capture.bSuccess);

    const TArray<TSharedPtr<FJsonValue>>* Notifies = nullptr;
    if (Capture.Result.IsValid())
    {
        TestTrue(TEXT("notifies array present"), Capture.Result->TryGetArrayField(TEXT("notifies"), Notifies));
    }
    if (!Notifies)
    {
        Sequence->RemoveFromRoot();
        return true;
    }
    TestEqual(TEXT("notify count"), Notifies->Num(), 3);
    if (Notifies->Num() != 3)
    {
        Sequence->RemoveFromRoot();
        return true;
    }

    TArray<TSharedPtr<FJsonObject>> NotifyObjects;
    for (const TSharedPtr<FJsonValue>& NotifyValue : *Notifies)
    {
        const TSharedPtr<FJsonObject> NotifyObject = NotifyValue.IsValid() ? NotifyValue->AsObject() : nullptr;
        TestTrue(TEXT("notify is an object"), NotifyObject.IsValid());
        if (NotifyObject.IsValid())
        {
            TestTrue(TEXT("notify name field present"), NotifyObject->HasField(TEXT("name")));
            TestTrue(TEXT("notify time field present"), NotifyObject->HasField(TEXT("time")));
            TestTrue(TEXT("notify duration field present"), NotifyObject->HasField(TEXT("duration")));
            TestTrue(TEXT("notify branchingPoint field present"), NotifyObject->HasField(TEXT("branchingPoint")));
            NotifyObjects.Add(NotifyObject);
        }
    }

    TestEqual(TEXT("valid notify object count"), NotifyObjects.Num(), 3);
    if (NotifyObjects.Num() != 3)
    {
        Sequence->RemoveFromRoot();
        return true;
    }

    TestEqual(TEXT("first notify sorted by time"), NotifyObjects[0]->GetStringField(TEXT("name")), FString(TEXT("Bravo")));
    TestEqual(TEXT("second notify sorted by name tie-break"), NotifyObjects[1]->GetStringField(TEXT("name")), FString(TEXT("Alpha")));
    TestEqual(TEXT("third notify sorted by name tie-break"), NotifyObjects[2]->GetStringField(TEXT("name")), FString(TEXT("Zed")));
    TestEqual(TEXT("first notify time"), static_cast<float>(NotifyObjects[0]->GetNumberField(TEXT("time"))), 0.10f);
    TestEqual(TEXT("second notify duration"), static_cast<float>(NotifyObjects[1]->GetNumberField(TEXT("duration"))), 0.15f);
    TestFalse(TEXT("sequence notify is not a branching point"), NotifyObjects[0]->GetBoolField(TEXT("branchingPoint")));
    Sequence->RemoveFromRoot();
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAuthoringSetNotifyStatePropertySetsNestedObjectPathTest,
    "PinWright.animation.authoring.set_notify_state_property.SetsNestedObjectPath",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FAuthoringSetNotifyStatePropertySetsNestedObjectPathTest::RunTest(const FString& Parameters)
{
    PinWrightAnimationTestFixtures::FRegisteredAnimationFixture Fixture;
    TestTrue(TEXT("registered animation fixture created"),
        Fixture.Create(TEXT("AS_SetNotifyStateProperty")));
    UAnimSequence* Sequence = Fixture.Sequence;
    const FString ObjectPath = Fixture.SequenceObjectPath;
    TestNotNull(TEXT("registered sequence created"), Sequence);
    if (!Sequence)
    {
        return false;
    }

    UTestAnimationNotifyStateWithPayload* NotifyState = NewObject<UTestAnimationNotifyStateWithPayload>(Sequence);
    TestNotNull(TEXT("fixture notify state created"), NotifyState);
    if (!NotifyState)
    {
        Sequence->RemoveFromRoot();
        return false;
    }

    NotifyState->Payload = NewObject<UTestAnimationNotifyStatePayload>(NotifyState);
    TestNotNull(TEXT("fixture payload created"), NotifyState->Payload.Get());
    if (!NotifyState->Payload)
    {
        Sequence->RemoveFromRoot();
        return false;
    }

    FAnimNotifyEvent& Event = Sequence->Notifies.AddDefaulted_GetRef();
    Event.NotifyStateClass = NotifyState;
    Event.NotifyName = FName(TEXT("WarpWindow"));
    Event.TriggerTimeOffset = 0.0f;
    Event.SetDuration(0.25f);

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), ObjectPath);
    Payload->SetNumberField(TEXT("notifyIndex"), 0);
    Payload->SetStringField(TEXT("propertyPath"), TEXT("Payload.WarpTargetName"));
    Payload->SetStringField(TEXT("value"), TEXT("AttackTarget"));
    Payload->SetBoolField(TEXT("save"), false);

    FTestResponseCapture Capture;
    TestTrue(TEXT("animation.authoring.set_notify_state_property handler found"),
        InvokeHandlerWithCapture(TEXT("animation.authoring.set_notify_state_property"), Payload, Capture));
    TestTrue(TEXT("handler sent success"), Capture.bWasCalled && Capture.bSuccess);
    // Counterfactual: without the production nested-path setter, this remains None.
    TestEqual(TEXT("Payload.WarpTargetName set through production handler"),
        NotifyState->Payload->WarpTargetName.ToString(), FString(TEXT("AttackTarget")));

    Sequence->RemoveFromRoot();
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAuthoringListSyncMarkersReturnsSortedDumpParityShapeTest,
    "PinWright.animation.authoring.list_sync_markers.ReturnsSortedDumpParityShape",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FAuthoringListSyncMarkersReturnsSortedDumpParityShapeTest::RunTest(const FString& Parameters)
{
    const FHandlerRegistration* Registration = nullptr;
    for (const FHandlerRegistration& Reg : FAutoRegisterHandler::GetPendingRegistrations())
    {
        if (Reg.MethodName == TEXT("animation.authoring.list_sync_markers"))
        {
            Registration = &Reg;
            break;
        }
    }

    TestNotNull(TEXT("animation.authoring.list_sync_markers handler registered"), Registration);
    if (!Registration)
    {
        return true;
    }

    TestEqual(TEXT("animation.authoring.list_sync_markers category"), Registration->Category, FString(TEXT("animation.authoring")));
    bool bHasRequiredAssetPath = false;
    for (const FParamSpec& Param : Registration->Params)
    {
        if (Param.Name == TEXT("assetPath") && Param.Type == TEXT("path") && Param.bRequired)
        {
            bHasRequiredAssetPath = true;
            break;
        }
    }
    TestTrue(TEXT("assetPath is required"), bHasRequiredAssetPath);

    PinWrightAnimationTestFixtures::FRegisteredAnimationFixture Fixture;
    TestTrue(TEXT("registered animation fixture created"),
        Fixture.Create(TEXT("AS_ListSyncMarkers")));
    UAnimSequence* Sequence = Fixture.Sequence;
    const FString ObjectPath = Fixture.SequenceObjectPath;
    TestNotNull(TEXT("registered sequence created"), Sequence);
    if (!Sequence)
    {
        return true;
    }

    auto AddSyncMarker = [Sequence](const TCHAR* MarkerName, float Time)
    {
        FAnimSyncMarker Marker;
        Marker.MarkerName = FName(MarkerName);
        Marker.Time = Time;
        Sequence->AuthoredSyncMarkers.Add(Marker);
    };

    AddSyncMarker(TEXT("B"), 0.5f);
    AddSyncMarker(TEXT("A"), 0.5f);
    AddSyncMarker(TEXT("Early"), 0.1f);

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), ObjectPath);

    FTestResponseCapture Capture;
    TestTrue(TEXT("animation.authoring.list_sync_markers handler found"),
        InvokeHandlerWithCapture(TEXT("animation.authoring.list_sync_markers"), Payload, Capture));
    TestTrue(TEXT("list_sync_markers succeeds"), Capture.bSuccess);

    const TArray<TSharedPtr<FJsonValue>>* SyncMarkers = nullptr;
    if (Capture.Result.IsValid())
    {
        TestTrue(TEXT("syncMarkers array present"), Capture.Result->TryGetArrayField(TEXT("syncMarkers"), SyncMarkers));
    }
    if (!SyncMarkers)
    {
        Sequence->RemoveFromRoot();
        return true;
    }
    TestEqual(TEXT("sync marker count"), SyncMarkers->Num(), 3);
    if (SyncMarkers->Num() != 3)
    {
        Sequence->RemoveFromRoot();
        return true;
    }

    TArray<TSharedPtr<FJsonObject>> SyncMarkerObjects;
    for (const TSharedPtr<FJsonValue>& SyncMarkerValue : *SyncMarkers)
    {
        const TSharedPtr<FJsonObject> SyncMarkerObject = SyncMarkerValue.IsValid() ? SyncMarkerValue->AsObject() : nullptr;
        TestTrue(TEXT("sync marker is an object"), SyncMarkerObject.IsValid());
        if (SyncMarkerObject.IsValid())
        {
            TestTrue(TEXT("sync marker name field present"), SyncMarkerObject->HasField(TEXT("name")));
            TestTrue(TEXT("sync marker time field present"), SyncMarkerObject->HasField(TEXT("time")));
            SyncMarkerObjects.Add(SyncMarkerObject);
        }
    }

    TestEqual(TEXT("valid sync marker object count"), SyncMarkerObjects.Num(), 3);
    if (SyncMarkerObjects.Num() != 3)
    {
        Sequence->RemoveFromRoot();
        return true;
    }

    TestEqual(TEXT("first sync marker sorted by time"), SyncMarkerObjects[0]->GetStringField(TEXT("name")), FString(TEXT("Early")));
    TestEqual(TEXT("second sync marker sorted by name tie-break"), SyncMarkerObjects[1]->GetStringField(TEXT("name")), FString(TEXT("A")));
    TestEqual(TEXT("third sync marker sorted by name tie-break"), SyncMarkerObjects[2]->GetStringField(TEXT("name")), FString(TEXT("B")));
    TestEqual(TEXT("first sync marker time"), static_cast<float>(SyncMarkerObjects[0]->GetNumberField(TEXT("time"))), 0.1f);
    TestEqual(TEXT("second sync marker time"), static_cast<float>(SyncMarkerObjects[1]->GetNumberField(TEXT("time"))), 0.5f);
    Sequence->RemoveFromRoot();
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAuthoringSyncMarkerMutationTest,
    "PinWright.animation.authoring.sync_markers.ReplaceRemoveAndRefreshDerivedState",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAuthoringSyncMarkerMutationTest::RunTest(const FString& Parameters)
{
    PinWrightAnimationTestFixtures::FRegisteredAnimationFixture Fixture;
    TestTrue(TEXT("registered animation fixture created"),
        Fixture.Create(TEXT("AS_SyncMarkerMutation")));
    UAnimSequence* Sequence = Fixture.Sequence;
    const FString ObjectPath = Fixture.SequenceObjectPath;
    TestNotNull(TEXT("registered sequence for sync-marker mutation created"), Sequence);
    if (!Sequence)
    {
        return false;
    }

    auto MakeMarker = [](const TCHAR* Name, int32 Frame)
    {
        TSharedPtr<FJsonObject> Marker = MakeShared<FJsonObject>();
        Marker->SetStringField(TEXT("name"), Name);
        Marker->SetNumberField(TEXT("frame"), Frame);
        return MakeShared<FJsonValueObject>(Marker);
    };

    TSharedPtr<FJsonObject> SetPayload = MakeShared<FJsonObject>();
    SetPayload->SetStringField(TEXT("assetPath"), ObjectPath);
    SetPayload->SetBoolField(TEXT("save"), false);
    SetPayload->SetArrayField(TEXT("markers"), {
        MakeMarker(TEXT("L"), 0),
        MakeMarker(TEXT("R"), 1),
        MakeMarker(TEXT("L"), 2),
    });

    FTestResponseCapture SetCapture;
    TestTrue(TEXT("set_sync_markers handler is registered"),
        InvokeHandlerWithCapture(TEXT("animation.authoring.set_sync_markers"),
            SetPayload, SetCapture));
    TestTrue(*FString::Printf(TEXT("set_sync_markers succeeds: %s %s"),
        *SetCapture.ErrorCode, *SetCapture.Message), SetCapture.bSuccess);
    TestEqual(TEXT("set_sync_markers authors duplicate labels without collapsing them"),
        Sequence->AuthoredSyncMarkers.Num(), 3);
    TestEqual(TEXT("set_sync_markers rebuilds unique marker names"),
        Sequence->UniqueMarkerNames.Num(), 2);
    TestTrue(TEXT("set_sync_markers creates a derived notify track"),
        Sequence->AnimNotifyTracks.Num() > 0);
    if (Sequence->AnimNotifyTracks.Num() > 0)
    {
        TestEqual(TEXT("set_sync_markers links every authored marker to its track"),
            Sequence->AnimNotifyTracks[0].SyncMarkers.Num(), 3);
    }

    // A bad replacement must be rejected before the existing set is touched.
    TSharedPtr<FJsonObject> InvalidPayload = MakeShared<FJsonObject>();
    InvalidPayload->SetStringField(TEXT("assetPath"), ObjectPath);
    InvalidPayload->SetBoolField(TEXT("save"), false);
    InvalidPayload->SetArrayField(TEXT("markers"), { MakeMarker(TEXT("Late"), 3) });
    FTestResponseCapture InvalidCapture;
    TestTrue(TEXT("invalid set_sync_markers handler is registered"),
        InvokeHandlerWithCapture(TEXT("animation.authoring.set_sync_markers"),
            InvalidPayload, InvalidCapture));
    TestFalse(TEXT("out-of-range RPC marker is refused"), InvalidCapture.bSuccess);
    TestEqual(TEXT("out-of-range RPC marker uses ANIMATION_INVALID"),
        InvalidCapture.ErrorCode, FString(TEXT("ANIMATION_INVALID")));
    TestEqual(TEXT("rejected replacement leaves the old marker set intact"),
        Sequence->AuthoredSyncMarkers.Num(), 3);

    TSharedPtr<FJsonObject> MovePayload = MakeShared<FJsonObject>();
    MovePayload->SetStringField(TEXT("assetPath"), ObjectPath);
    MovePayload->SetBoolField(TEXT("save"), false);
    MovePayload->SetArrayField(TEXT("markers"), {
        MakeMarker(TEXT("L"), 0),
        MakeMarker(TEXT("R"), 2),
        MakeMarker(TEXT("L"), 1),
    });
    FTestResponseCapture MoveCapture;
    TestTrue(TEXT("replacement handler is registered"),
        InvokeHandlerWithCapture(TEXT("animation.authoring.set_sync_markers"),
            MovePayload, MoveCapture));
    TestTrue(TEXT("replacement succeeds"), MoveCapture.bSuccess);
    TestEqual(TEXT("replacement changes the authored marker count"),
        Sequence->AuthoredSyncMarkers.Num(), 3);
    TestTrue(TEXT("replacement moves R to the final frame"),
        Sequence->AuthoredSyncMarkers.ContainsByPredicate(
            [](const FAnimSyncMarker& Marker)
            {
                return Marker.MarkerName == FName(TEXT("R"))
                    && FMath::IsNearlyEqual(Marker.Time, 2.0f / 30.0f);
            }));

    TSharedPtr<FJsonObject> RemovePayload = MakeShared<FJsonObject>();
    RemovePayload->SetStringField(TEXT("assetPath"), ObjectPath);
    RemovePayload->SetStringField(TEXT("markerName"), TEXT("L"));
    RemovePayload->SetBoolField(TEXT("save"), false);
    FTestResponseCapture RemoveCapture;
    TestTrue(TEXT("remove_sync_marker handler is registered"),
        InvokeHandlerWithCapture(TEXT("animation.authoring.remove_sync_marker"),
            RemovePayload, RemoveCapture));
    TestTrue(TEXT("remove_sync_marker succeeds"), RemoveCapture.bSuccess);
    if (RemoveCapture.bSuccess && RemoveCapture.Result.IsValid())
    {
        TestEqual(TEXT("remove response reports all duplicate marker occurrences"),
            static_cast<int32>(RemoveCapture.Result->GetNumberField(TEXT("removedCount"))), 2);
        TestTrue(TEXT("remove response includes measured marker readback"),
            RemoveCapture.Result->HasField(TEXT("syncMarkers")));
    }
    TestEqual(TEXT("remove leaves only the requested replacement marker"),
        Sequence->AuthoredSyncMarkers.Num(), 1);
    TestEqual(TEXT("remove refreshes unique marker names"),
        Sequence->UniqueMarkerNames.Num(), 1);
    Sequence->RemoveFromRoot();
    return true;
}

// Regression for B-create-montage-duplicate-default-slot: the UAnimMontage constructor
// (via UAnimMontageFactory) already seeds one track named "DefaultSlot", and the handler
// then unconditionally appended another, so a freshly-created montage carried TWO slots
// both named "DefaultSlot" (numSlots:2) before any add_montage_slot call — contradicting
// the documented "one slot" contract, and the duplicate was unremovable via the typed
// surface (there is no remove_montage_slot verb). The fix find-or-renames the seeded slot
// instead of appending. This test drives the real create_montage handler twice — once with
// the default slotName, once with a custom slotName — and asserts exactly one slot of the
// requested name in each case. Reverting the fix restores the unconditional append, both
// montages report two SlotAnimTracks, and these assertions fail.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAuthoringCreateMontageSingleSlotTest,
    "PinWright.animation.authoring.create_montage.SingleSlotNoDuplicate",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FAuthoringCreateMontageSingleSlotTest::RunTest(const FString& Parameters)
{
    FString SkeletonPath;
    USkeleton* Skeleton = NewLoadedTransientSkeleton(TEXT("SK_MontageSingleSlot"), SkeletonPath);
    TestNotNull(TEXT("transient skeleton created"), Skeleton);
    if (!Skeleton)
    {
        return false;
    }

    // Helper: create a montage with the given slotName (empty -> omit, letting the handler
    // default to "DefaultSlot") and assert it ends with exactly one slot of the expected name.
    auto CheckSingleSlot = [&](const FString& RequestedSlotName, const FString& ExpectedSlotName)
    {
        const FString MontageName = FString::Printf(
            TEXT("MTG_SingleSlot_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
        const FString MontagePackagePath = FString::Printf(
            TEXT("/Game/PinWrightTests/%s"), *MontageName);
        const FString MontageObjectPath = FString::Printf(
            TEXT("%s.%s"), *MontagePackagePath, *MontageName);

        TSharedPtr<FJsonObject> CreatePayload = MakeShared<FJsonObject>();
        CreatePayload->SetStringField(TEXT("name"), MontageName);
        CreatePayload->SetStringField(TEXT("path"), TEXT("/Game/PinWrightTests"));
        CreatePayload->SetStringField(TEXT("skeletonPath"), SkeletonPath);
        if (!RequestedSlotName.IsEmpty())
        {
            CreatePayload->SetStringField(TEXT("slotName"), RequestedSlotName);
        }
        CreatePayload->SetBoolField(TEXT("save"), false);

        FTestResponseCapture CreateCapture;
        TestTrue(TEXT("animation.authoring.create_montage handler found"),
            InvokeHandlerWithCapture(TEXT("animation.authoring.create_montage"), CreatePayload, CreateCapture));
        TestTrue(TEXT("create_montage succeeds"), CreateCapture.bWasCalled && CreateCapture.bSuccess);

        UAnimMontage* Montage = Cast<UAnimMontage>(
            StaticLoadObject(UAnimMontage::StaticClass(), nullptr, *MontageObjectPath));
        TestNotNull(TEXT("created montage loads"), Montage);
        if (Montage)
        {
            // Core assertion: exactly one slot, named as requested. Pre-fix the constructor-seeded
            // "DefaultSlot" plus the handler-appended slot made this 2.
            TestEqual(TEXT("create_montage leaves exactly one slot"),
                Montage->SlotAnimTracks.Num(), 1);
            if (Montage->SlotAnimTracks.Num() == 1)
            {
                TestEqual(TEXT("the single slot carries the requested name"),
                    Montage->SlotAnimTracks[0].SlotName, FName(*ExpectedSlotName));
            }
        }
        CleanupTestAsset(MontagePackagePath);
    };

    // Default path: slotName omitted -> handler defaults to "DefaultSlot", which collides with
    // the factory-seeded slot. Pre-fix this was the duplicate-"DefaultSlot" repro.
    CheckSingleSlot(FString(), TEXT("DefaultSlot"));
    // Custom path: a non-default slotName must NOT leave the seeded "DefaultSlot" behind alongside
    // the requested one — the seeded track is renamed, yielding a single "UpperBody" slot.
    CheckSingleSlot(TEXT("UpperBody"), TEXT("UpperBody"));

    Skeleton->RemoveFromRoot();
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAuthoringCompositeCreateAndAddSegmentTest,
    "PinWright.animation.authoring.create_composite.CreateAndAddSegment",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FAuthoringCompositeCreateAndAddSegmentTest::RunTest(const FString& Parameters)
{
    FString SkeletonPath;
    USkeleton* Skeleton = NewLoadedTransientSkeleton(TEXT("SK_Composite"), SkeletonPath);
    TestNotNull(TEXT("transient skeleton created"), Skeleton);
    if (!Skeleton)
    {
        return false;
    }

    FString SequencePath;
    UAnimSequence* Sequence = NewLoadedTransientAnimSequence(
        TEXT("AS_CompositeSegment"), SequencePath, FFrameRate(30, 1), Skeleton);
    TestNotNull(TEXT("transient sequence created"), Sequence);
    if (!Sequence)
    {
        Skeleton->RemoveFromRoot();
        return false;
    }

    const FString CompositeName = FString::Printf(
        TEXT("AC_Composite_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    const FString CompositePackagePath = FString::Printf(
        TEXT("/Game/PinWrightTests/%s"), *CompositeName);
    const FString CompositeObjectPath = FString::Printf(
        TEXT("%s.%s"), *CompositePackagePath, *CompositeName);

    TSharedPtr<FJsonObject> CreatePayload = MakeShared<FJsonObject>();
    CreatePayload->SetStringField(TEXT("name"), CompositeName);
    CreatePayload->SetStringField(TEXT("path"), TEXT("/Game/PinWrightTests"));
    CreatePayload->SetStringField(TEXT("skeletonPath"), SkeletonPath);
    CreatePayload->SetBoolField(TEXT("save"), false);

    FTestResponseCapture CreateCapture;
    TestTrue(TEXT("animation.authoring.create_composite handler found"),
        InvokeHandlerWithCapture(TEXT("animation.authoring.create_composite"), CreatePayload, CreateCapture));
    TestTrue(TEXT("create_composite succeeds"), CreateCapture.bWasCalled && CreateCapture.bSuccess);

    UAnimComposite* Composite = Cast<UAnimComposite>(
        StaticLoadObject(UAnimComposite::StaticClass(), nullptr, *CompositeObjectPath));
    TestNotNull(TEXT("created composite loads"), Composite);
    if (!Composite)
    {
        Sequence->RemoveFromRoot();
        Skeleton->RemoveFromRoot();
        return false;
    }
    TestTrue(TEXT("created composite uses requested skeleton"), Composite->GetSkeleton() == Skeleton);

    TSharedPtr<FJsonObject> SegmentPayload = MakeShared<FJsonObject>();
    SegmentPayload->SetStringField(TEXT("assetPath"), CompositeObjectPath);
    SegmentPayload->SetStringField(TEXT("animationPath"), SequencePath);
    SegmentPayload->SetBoolField(TEXT("save"), false);

    FTestResponseCapture SegmentCapture;
    TestTrue(TEXT("animation.authoring.add_composite_segment handler found"),
        InvokeHandlerWithCapture(TEXT("animation.authoring.add_composite_segment"), SegmentPayload, SegmentCapture));
    TestTrue(TEXT("add_composite_segment succeeds"), SegmentCapture.bWasCalled && SegmentCapture.bSuccess);
    if (SegmentCapture.Result.IsValid())
    {
        TestEqual(TEXT("segmentIndex returned"), static_cast<int32>(SegmentCapture.Result->GetNumberField(TEXT("segmentIndex"))), 0);
        TestEqual(TEXT("segmentCount returned"), static_cast<int32>(SegmentCapture.Result->GetNumberField(TEXT("segmentCount"))), 1);
    }

    TestEqual(TEXT("composite segment count"), Composite->AnimationTrack.AnimSegments.Num(), 1);
    if (Composite->AnimationTrack.AnimSegments.Num() == 1)
    {
        const FAnimSegment& Segment = Composite->AnimationTrack.AnimSegments[0];
        TestTrue(TEXT("segment references sequence"), Segment.GetAnimReference() == Sequence);
        TestTrue(TEXT("segment end defaults to sequence length"),
            FMath::IsNearlyEqual(Segment.AnimEndTime, Sequence->GetPlayLength()));
    }

    if (Composite->AnimationTrack.AnimSegments.Num() == 1)
    {
        Composite->AnimationTrack.AnimSegments[0].StartPos = 10.0f;
        Composite->SetCompositeLength(Composite->AnimationTrack.GetLength());

        TSharedPtr<FJsonObject> InsertPayload = MakeShared<FJsonObject>();
        InsertPayload->SetStringField(TEXT("assetPath"), CompositeObjectPath);
        InsertPayload->SetStringField(TEXT("animationPath"), SequencePath);
        InsertPayload->SetNumberField(TEXT("startPos"), 1.0);
        InsertPayload->SetBoolField(TEXT("save"), false);

        FTestResponseCapture InsertCapture;
        TestTrue(TEXT("animation.authoring.add_composite_segment handler found for non-tail insert"),
            InvokeHandlerWithCapture(TEXT("animation.authoring.add_composite_segment"), InsertPayload, InsertCapture));
        TestTrue(TEXT("non-tail add_composite_segment succeeds"), InsertCapture.bWasCalled && InsertCapture.bSuccess);
        if (InsertCapture.Result.IsValid())
        {
            TestEqual(TEXT("inserted segmentIndex returned"), static_cast<int32>(InsertCapture.Result->GetNumberField(TEXT("segmentIndex"))), 0);
            TestEqual(TEXT("inserted segmentCount returned"), static_cast<int32>(InsertCapture.Result->GetNumberField(TEXT("segmentCount"))), 2);
            TestTrue(TEXT("returned compositeLength matches track length"),
                FMath::IsNearlyEqual(
                    static_cast<float>(InsertCapture.Result->GetNumberField(TEXT("compositeLength"))),
                    Composite->AnimationTrack.GetLength()));
        }
        TestEqual(TEXT("composite segment count after insert"), Composite->AnimationTrack.AnimSegments.Num(), 2);
        if (Composite->AnimationTrack.AnimSegments.Num() == 2)
        {
            TestTrue(TEXT("inserted segment is first after sort/validation"),
                Composite->AnimationTrack.AnimSegments[0].GetAnimReference() == Sequence);
            TestTrue(TEXT("existing segment follows inserted segment"),
                FMath::IsNearlyEqual(
                    Composite->AnimationTrack.AnimSegments[1].StartPos,
                    Composite->AnimationTrack.AnimSegments[0].GetEndPos()));
        }
    }

    Sequence->RemoveFromRoot();
    Skeleton->RemoveFromRoot();
    CleanupTestAsset(CompositePackagePath);
    return true;
}

// Regression for B-montage-slot-no-sequence-length-recalc: add_montage_slot used to append a
// slot segment but never recompute the montage's SequenceLength, so a montage stayed
// duration=0 (GetPlayLength()==0) even after a multi-second clip was added — breaking
// section/notify clamping, playback, and every read (get_animation_info). The fix recomputes
// the slot-segment-derived length via CalculateSequenceLength() and persists it through
// SetCompositeLength() before saving. This test drives the real handler and asserts the montage
// gains a non-zero play length matching the added clip; reverting the fix returns it to 0 and
// the assertion fails. SetCompositeLength frame-rounds in the editor (WITH_EDITOR) data model,
// so the length is asserted as >0 and approximately the clip length (within one frame), not bit-exact.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAuthoringAddMontageSlotRecomputesLengthTest,
    "PinWright.animation.authoring.add_montage_slot.RecomputesMontageLength",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FAuthoringAddMontageSlotRecomputesLengthTest::RunTest(const FString& Parameters)
{
    FString SkeletonPath;
    USkeleton* Skeleton = NewLoadedTransientSkeleton(TEXT("SK_MontageSlot"), SkeletonPath);
    TestNotNull(TEXT("transient skeleton created"), Skeleton);
    if (!Skeleton)
    {
        return false;
    }

    // 30fps x 2 frames -> ~0.0667s of real play length. Non-zero is the point.
    FString SequencePath;
    UAnimSequence* Sequence = NewLoadedTransientAnimSequence(
        TEXT("AS_MontageSlot"), SequencePath, FFrameRate(30, 1), Skeleton);
    TestNotNull(TEXT("transient sequence created"), Sequence);
    if (!Sequence)
    {
        Skeleton->RemoveFromRoot();
        return false;
    }
    const float ClipLength = Sequence->GetPlayLength();
    TestTrue(TEXT("source clip has a non-zero play length"), ClipLength > 0.0f);

    const FString MontageName = FString::Printf(
        TEXT("MTG_SlotLength_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    const FString MontagePackagePath = FString::Printf(
        TEXT("/Game/PinWrightTests/%s"), *MontageName);
    const FString MontageObjectPath = FString::Printf(
        TEXT("%s.%s"), *MontagePackagePath, *MontageName);

    TSharedPtr<FJsonObject> CreatePayload = MakeShared<FJsonObject>();
    CreatePayload->SetStringField(TEXT("name"), MontageName);
    CreatePayload->SetStringField(TEXT("path"), TEXT("/Game/PinWrightTests"));
    CreatePayload->SetStringField(TEXT("skeletonPath"), SkeletonPath);
    CreatePayload->SetStringField(TEXT("slotName"), TEXT("UpperBody"));
    CreatePayload->SetBoolField(TEXT("save"), false);

    FTestResponseCapture CreateCapture;
    TestTrue(TEXT("animation.authoring.create_montage handler found"),
        InvokeHandlerWithCapture(TEXT("animation.authoring.create_montage"), CreatePayload, CreateCapture));
    TestTrue(TEXT("create_montage succeeds"), CreateCapture.bWasCalled && CreateCapture.bSuccess);

    UAnimMontage* Montage = Cast<UAnimMontage>(
        StaticLoadObject(UAnimMontage::StaticClass(), nullptr, *MontageObjectPath));
    TestNotNull(TEXT("created montage loads"), Montage);
    if (!Montage)
    {
        Sequence->RemoveFromRoot();
        Skeleton->RemoveFromRoot();
        return false;
    }
    // Empty montage is correctly length 0 before any slot content is added.
    TestTrue(TEXT("empty montage starts at duration 0"),
        FMath::IsNearlyZero(Montage->GetPlayLength()));

    TSharedPtr<FJsonObject> SlotPayload = MakeShared<FJsonObject>();
    SlotPayload->SetStringField(TEXT("assetPath"), MontageObjectPath);
    SlotPayload->SetStringField(TEXT("animationPath"), SequencePath);
    SlotPayload->SetStringField(TEXT("slotName"), TEXT("UpperBody"));
    SlotPayload->SetBoolField(TEXT("save"), false);

    FTestResponseCapture SlotCapture;
    TestTrue(TEXT("animation.authoring.add_montage_slot handler found"),
        InvokeHandlerWithCapture(TEXT("animation.authoring.add_montage_slot"), SlotPayload, SlotCapture));
    TestTrue(TEXT("add_montage_slot succeeds"), SlotCapture.bWasCalled && SlotCapture.bSuccess);

    // Core assertion: the montage now has a real, non-zero length derived from the slot segment.
    // Pre-fix this stayed 0 (no length recompute) and both checks below failed.
    const float MontageLength = Montage->GetPlayLength();
    TestTrue(TEXT("montage gained a non-zero length after add_montage_slot"), MontageLength > 0.0f);
    // Frame-rounded to the montage's frame rate, so allow one frame of slack rather than bit-exact.
    TestTrue(TEXT("montage length approximately matches the added clip length"),
        FMath::IsNearlyEqual(MontageLength, ClipLength, 1.0f / 30.0f + KINDA_SMALL_NUMBER));

    // The handler also surfaces the recomputed length in its response payload.
    if (SlotCapture.Result.IsValid())
    {
        TestTrue(TEXT("response montageLength matches the montage play length"),
            FMath::IsNearlyEqual(
                static_cast<float>(SlotCapture.Result->GetNumberField(TEXT("montageLength"))),
                MontageLength));
    }

    Sequence->RemoveFromRoot();
    Skeleton->RemoveFromRoot();
    CleanupTestAsset(MontagePackagePath);
    return true;
}

// Regression for B-add-montage-notify-time-dropped: add_montage_notify accepted a `time`
// parameter and returned success, but wrote only FAnimNotifyEvent::TriggerTimeOffset and never
// called Link()/SetTime(), so the FAnimLinkableElement LinkValue stayed 0. The reader
// (list_notifies / asset.dump / the editor notify panel) reports GetTime() == LinkValue, so every
// notify landed at time 0 regardless of the requested time. The fix calls Link(Montage, Time) so
// GetTime() == Time, matching the canonical non-authoring animation.add_notify pattern. This test
// drives the real handler with time 0.5 and asserts both the live notify's GetTime() and the
// list_notifies reader report 0.5; reverting the fix returns both to 0 and the assertions fail.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAuthoringAddMontageNotifyAppliesTimeTest,
    "PinWright.animation.authoring.add_montage_notify.AppliesRequestedTime",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FAuthoringAddMontageNotifyAppliesTimeTest::RunTest(const FString& Parameters)
{
    FString SkeletonPath;
    USkeleton* Skeleton = NewLoadedTransientSkeleton(TEXT("SK_MontageNotifyTime"), SkeletonPath);
    TestNotNull(TEXT("transient skeleton created"), Skeleton);
    if (!Skeleton)
    {
        return false;
    }

    const FString MontageName = FString::Printf(
        TEXT("MTG_NotifyTime_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    const FString MontagePackagePath = FString::Printf(
        TEXT("/Game/PinWrightTests/%s"), *MontageName);
    const FString MontageObjectPath = FString::Printf(
        TEXT("%s.%s"), *MontagePackagePath, *MontageName);

    TSharedPtr<FJsonObject> CreatePayload = MakeShared<FJsonObject>();
    CreatePayload->SetStringField(TEXT("name"), MontageName);
    CreatePayload->SetStringField(TEXT("path"), TEXT("/Game/PinWrightTests"));
    CreatePayload->SetStringField(TEXT("skeletonPath"), SkeletonPath);
    CreatePayload->SetStringField(TEXT("slotName"), TEXT("UpperBody"));
    CreatePayload->SetBoolField(TEXT("save"), false);

    FTestResponseCapture CreateCapture;
    TestTrue(TEXT("animation.authoring.create_montage handler found"),
        InvokeHandlerWithCapture(TEXT("animation.authoring.create_montage"), CreatePayload, CreateCapture));
    TestTrue(TEXT("create_montage succeeds"), CreateCapture.bWasCalled && CreateCapture.bSuccess);

    UAnimMontage* Montage = Cast<UAnimMontage>(
        StaticLoadObject(UAnimMontage::StaticClass(), nullptr, *MontageObjectPath));
    TestNotNull(TEXT("created montage loads"), Montage);
    if (!Montage)
    {
        Skeleton->RemoveFromRoot();
        return false;
    }

    const float RequestedTime = 0.5f;
    TSharedPtr<FJsonObject> NotifyPayload = MakeShared<FJsonObject>();
    NotifyPayload->SetStringField(TEXT("assetPath"), MontageObjectPath);
    NotifyPayload->SetNumberField(TEXT("time"), RequestedTime);
    NotifyPayload->SetStringField(TEXT("notifyName"), TEXT("ImpactFX"));
    NotifyPayload->SetBoolField(TEXT("save"), false);

    FTestResponseCapture NotifyCapture;
    TestTrue(TEXT("animation.authoring.add_montage_notify handler found"),
        InvokeHandlerWithCapture(TEXT("animation.authoring.add_montage_notify"), NotifyPayload, NotifyCapture));
    TestTrue(TEXT("add_montage_notify succeeds"), NotifyCapture.bWasCalled && NotifyCapture.bSuccess);

    // Core assertion #1: the live notify's GetTime() (== LinkValue) must equal the requested time.
    // Pre-fix only TriggerTimeOffset was written and LinkValue stayed 0, so GetTime() returned 0.
    TestEqual(TEXT("montage gained exactly one notify"), Montage->Notifies.Num(), 1);
    if (Montage->Notifies.Num() == 1)
    {
        TestTrue(TEXT("notify GetTime() matches the requested time"),
            FMath::IsNearlyEqual(Montage->Notifies[0].GetTime(), RequestedTime, KINDA_SMALL_NUMBER));
    }

    // Core assertion #2: the production reader (list_notifies) reports the requested time, not 0.
    TSharedPtr<FJsonObject> ListPayload = MakeShared<FJsonObject>();
    ListPayload->SetStringField(TEXT("assetPath"), MontageObjectPath);

    FTestResponseCapture ListCapture;
    TestTrue(TEXT("animation.authoring.list_notifies handler found"),
        InvokeHandlerWithCapture(TEXT("animation.authoring.list_notifies"), ListPayload, ListCapture));
    TestTrue(TEXT("list_notifies succeeds"), ListCapture.bSuccess);

    const TArray<TSharedPtr<FJsonValue>>* Notifies = nullptr;
    if (ListCapture.Result.IsValid())
    {
        TestTrue(TEXT("notifies array present"), ListCapture.Result->TryGetArrayField(TEXT("notifies"), Notifies));
    }
    if (Notifies && Notifies->Num() == 1)
    {
        const TSharedPtr<FJsonObject> NotifyObject = (*Notifies)[0]->AsObject();
        TestTrue(TEXT("notify is an object"), NotifyObject.IsValid());
        if (NotifyObject.IsValid())
        {
            TestEqual(TEXT("list_notifies reports the requested notify name"),
                NotifyObject->GetStringField(TEXT("name")), FString(TEXT("ImpactFX")));
            TestTrue(TEXT("list_notifies reports the requested time, not 0"),
                FMath::IsNearlyEqual(
                    static_cast<float>(NotifyObject->GetNumberField(TEXT("time"))),
                    RequestedTime, KINDA_SMALL_NUMBER));
        }
    }
    else
    {
        TestEqual(TEXT("list_notifies returns exactly one notify"), Notifies ? Notifies->Num() : 0, 1);
    }

    Skeleton->RemoveFromRoot();
    CleanupTestAsset(MontagePackagePath);
    return true;
}

// Regression for B-tests-order-dependent-ensure-masking: add_montage_notify with the default
// notifyClass resolved the synthetic "AnimNotify_AnimNotify" name, missed, and fell back to
// UAnimNotify::StaticClass() -- which is UCLASS(abstract). NewObject<UAnimNotify> on an abstract
// class trips the editor ensure in StaticAllocateObjectErrorTests (UObjectGlobals.cpp) and the
// object is nulled out on save (silent data loss while the handler still reports success). This
// asserts the ORDER-INDEPENDENT root-cause invariant -- the notify the handler attaches must
// never be an abstract-class object -- rather than depending on the one-shot ensure that earlier
// tests disarm in full-suite runs. Pre-fix the attached notify's class is the abstract UAnimNotify
// so this fails deterministically; the fix attaches a valid name-only notify (null object) instead.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAuthoringAddMontageNotifyNotAbstractTest,
    "PinWright.animation.authoring.add_montage_notify.DoesNotInstantiateAbstractNotify",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FAuthoringAddMontageNotifyNotAbstractTest::RunTest(const FString& Parameters)
{
    // Exercise a real host-shipped montage. Host-dependent fixture: when NONE of the
    // candidates' packages exist in this host project, skip (FIXTURE-SKIP); when at
    // least one package exists but fails to load below, that stays a hard FAILURE.
    const TArray<FString> Candidates = {
        TEXT("/Game/Characters/Mannequins/Animations/Manny/MM_Rifle_Reload_Montage.MM_Rifle_Reload_Montage"),
        TEXT("/Game/Characters/Mannequins/Animations/Manny/MM_Rifle_Melee_Montage.MM_Rifle_Melee_Montage"),
        TEXT("/Game/Characters/Mannequins/Animations/Manny/MM_HitReact_Front_Med_01_Additive_Montage.MM_HitReact_Front_Med_01_Additive_Montage"),
        TEXT("/Game/ExampleContent/ControlRig/Animations/Punch_Montage.Punch_Montage"),
    };
    PINWRIGHT_SKIP_IF_ALL_FIXTURES_MISSING(Candidates);

    UAnimMontage* Montage = nullptr;
    FString MontagePath;
    for (const FString& Candidate : Candidates)
    {
        Montage = Cast<UAnimMontage>(StaticLoadObject(UAnimMontage::StaticClass(), nullptr, *Candidate));
        if (Montage)
        {
            MontagePath = Candidate;
            break;
        }
    }
    TestNotNull(TEXT("a real montage fixture loaded"), Montage);
    if (!Montage)
    {
        return false;
    }

    const int32 NumBefore = Montage->Notifies.Num();

    // Default notifyClass (omitted) -> handler must resolve a concrete class or a name-only
    // notify, never the abstract UAnimNotify base.
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), MontagePath);
    Payload->SetNumberField(TEXT("time"), 0.1);
    Payload->SetStringField(TEXT("notifyName"), TEXT("PinWrightAbstractProbe"));
    Payload->SetBoolField(TEXT("save"), false);

    FTestResponseCapture Capture;
    TestTrue(TEXT("animation.authoring.add_montage_notify handler found"),
        InvokeHandlerWithCapture(TEXT("animation.authoring.add_montage_notify"), Payload, Capture));
    TestTrue(TEXT("add_montage_notify succeeds"), Capture.bWasCalled && Capture.bSuccess);

    // Non-vacuous: the call must have added exactly one notify, and that notify's object (if
    // any) must not be of an abstract class. Pre-fix the object is a non-null abstract
    // UAnimNotify; post-fix it is null (a valid name-only notify) -> the check passes.
    TestEqual(TEXT("add_montage_notify added exactly one notify"), Montage->Notifies.Num(), NumBefore + 1);
    if (Montage->Notifies.Num() == NumBefore + 1)
    {
        const UAnimNotify* Added = Montage->Notifies[NumBefore].Notify;
        const bool bAbstract = (Added != nullptr) && Added->GetClass()->HasAnyClassFlags(CLASS_Abstract);
        TestFalse(TEXT("attached notify object must not be an abstract class"), bAbstract);
    }

    // Restore the shared content fixture (save=false, so nothing persisted; undo the in-memory
    // mutation so nothing downstream in-session observes the extra notify).
    if (Montage->Notifies.Num() > NumBefore)
    {
        Montage->Notifies.RemoveAt(NumBefore, Montage->Notifies.Num() - NumBefore);
        Montage->RefreshCacheData();
    }
    if (UPackage* Pkg = Montage->GetOutermost())
    {
        Pkg->SetDirtyFlag(false);
    }
    return true;
}

// Regression for B-tests-order-dependent-ensure-masking (track-growth clamp): the new
// AnimationAuthoringHelpers::EnsureNotifyTrack grows AnimNotifyTracks until the event's index is
// valid. A client/fuzzer-supplied out-of-range trackIndex (e.g. 1000000) must NOT drive that loop
// to allocate ~a million tracks -- the engine's UAnimSequenceBase::RefreshCacheData clamps any
// index < 0 or > 20 to 0 before growing, and this helper must mirror that. Post-fix an out-of-range
// index clamps to 0 so track growth stays bounded; pre-fix (upper clamp missing) the track list
// would explode. Asserts the bounded-growth invariant on a real montage fixture.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAuthoringAddMontageNotifyClampsHugeTrackIndexTest,
    "PinWright.animation.authoring.add_montage_notify.ClampsOutOfRangeTrackIndex",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FAuthoringAddMontageNotifyClampsHugeTrackIndexTest::RunTest(const FString& Parameters)
{
    // Host-dependent fixture: when NONE of the candidates' packages exist in this host
    // project, skip (FIXTURE-SKIP); an existing package that fails to load stays a hard FAILURE.
    const TArray<FString> Candidates = {
        TEXT("/Game/Characters/Mannequins/Animations/Manny/MM_Rifle_Reload_Montage.MM_Rifle_Reload_Montage"),
        TEXT("/Game/Characters/Mannequins/Animations/Manny/MM_Rifle_Melee_Montage.MM_Rifle_Melee_Montage"),
        TEXT("/Game/Characters/Mannequins/Animations/Manny/MM_HitReact_Front_Med_01_Additive_Montage.MM_HitReact_Front_Med_01_Additive_Montage"),
        TEXT("/Game/ExampleContent/ControlRig/Animations/Punch_Montage.Punch_Montage"),
    };
    PINWRIGHT_SKIP_IF_ALL_FIXTURES_MISSING(Candidates);

    UAnimMontage* Montage = nullptr;
    FString MontagePath;
    for (const FString& Candidate : Candidates)
    {
        Montage = Cast<UAnimMontage>(StaticLoadObject(UAnimMontage::StaticClass(), nullptr, *Candidate));
        if (Montage)
        {
            MontagePath = Candidate;
            break;
        }
    }
    TestNotNull(TEXT("a real montage fixture loaded"), Montage);
    if (!Montage)
    {
        return false;
    }

    const int32 NumNotifiesBefore = Montage->Notifies.Num();
    const int32 NumTracksBefore = Montage->AnimNotifyTracks.Num();

    // A wildly out-of-range trackIndex the fuzzer readily generates.
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), MontagePath);
    Payload->SetNumberField(TEXT("time"), 0.1);
    Payload->SetNumberField(TEXT("trackIndex"), 1000000);
    Payload->SetStringField(TEXT("notifyName"), TEXT("PinWrightHugeTrackProbe"));
    Payload->SetBoolField(TEXT("save"), false);

    FTestResponseCapture Capture;
    TestTrue(TEXT("animation.authoring.add_montage_notify handler found"),
        InvokeHandlerWithCapture(TEXT("animation.authoring.add_montage_notify"), Payload, Capture));
    TestTrue(TEXT("add_montage_notify succeeds"), Capture.bWasCalled && Capture.bSuccess);

    // Bounded-growth invariant: an out-of-range index clamps to 0, so the track list grows by at
    // most the engine's ~21-track ceiling -- never toward the requested 1,000,000. Pre-fix the
    // upper clamp is missing and this delta blows past the bound (a DoS/OOM).
    TestTrue(TEXT("add_montage_notify added exactly one notify"),
        Montage->Notifies.Num() == NumNotifiesBefore + 1);
    TestTrue(TEXT("out-of-range trackIndex must not grow AnimNotifyTracks unboundedly"),
        Montage->AnimNotifyTracks.Num() <= NumTracksBefore + 21);

    // Restore the shared content fixture (save=false; undo the in-memory mutation).
    if (Montage->Notifies.Num() > NumNotifiesBefore)
    {
        Montage->Notifies.RemoveAt(NumNotifiesBefore, Montage->Notifies.Num() - NumNotifiesBefore);
        Montage->RefreshCacheData();
    }
    if (UPackage* Pkg = Montage->GetOutermost())
    {
        Pkg->SetDirtyFlag(false);
    }
    return true;
}

// Fixed-behavior regression for the set_interpolation_settings B9 defect: the
// documented interpolationType param was read into a dead local (never applied),
// while TargetWeightInterpolationSpeedPerSec was clobbered to the default 5.0 on
// every call. The fix writes the parsed EFilterInterpolationType into every
// InterpolationParam axis and only touches the speed when the caller supplies it.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAuthoringSetInterpolationSettingsAppliesTypeTest,
    "PinWright.animation.authoring.set_interpolation_settings.AppliesInterpolationType",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FAuthoringSetInterpolationSettingsAppliesTypeTest::RunTest(const FString& Parameters)
{
    FString ObjectPath;
    UBlendSpace1D* BlendSpace = NewLoadedTransientBlendSpace1D(TEXT("BS_SetInterpType"), ObjectPath);
    TestNotNull(TEXT("transient blend space created"), BlendSpace);
    if (!BlendSpace)
    {
        return true;
    }

    // Seed a distinct speed the caller will NOT ask to change, so we can prove the
    // fix leaves it untouched instead of resetting it to the default 5.0.
    BlendSpace->TargetWeightInterpolationSpeedPerSec = 3.0f;

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), ObjectPath);
    Payload->SetStringField(TEXT("interpolationType"), TEXT("Cubic"));
    Payload->SetBoolField(TEXT("save"), false);

    FTestResponseCapture Capture;
    TestTrue(TEXT("animation.authoring.set_interpolation_settings handler found"),
        InvokeHandlerWithCapture(TEXT("animation.authoring.set_interpolation_settings"), Payload, Capture));
    TestTrue(TEXT("set_interpolation_settings succeeds"), Capture.bSuccess);

    // interpolationType='Cubic' must land on every input axis (previously a dead local).
    TestEqual(TEXT("axis 0 interpolation type is Cubic"),
        static_cast<int32>(BlendSpace->InterpolationParam[0].InterpolationType.GetValue()),
        static_cast<int32>(BSIT_Cubic));

    // The unspecified speed must be preserved, not clobbered to 5.0.
    TestEqual(TEXT("target weight speed left untouched"),
        BlendSpace->TargetWeightInterpolationSpeedPerSec, 3.0f);

    BlendSpace->RemoveFromRoot();
    return true;
}

// Fixed-behavior regression for the add_slot_node B7 defect: the handler baked
// "GroupName.SlotName" into Node.SlotName (so no montage slot named 'DefaultSlot'
// could ever match it) and never registered the group. The fix stores the BARE
// slot name and registers the group at the skeleton level via SetSlotGroupName.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAuthoringAddSlotNodeBareNameAndGroupTest,
    "PinWright.animation.authoring.add_slot_node.BareSlotNameAndGroup",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FAuthoringAddSlotNodeBareNameAndGroupTest::RunTest(const FString& Parameters)
{
    FString SkeletonPath;
    USkeleton* Skeleton = NewLoadedTransientSkeleton(TEXT("SK_AddSlotNode"), SkeletonPath);
    TestNotNull(TEXT("transient skeleton created"), Skeleton);
    if (!Skeleton)
    {
        return true;
    }

    const FString AssetName = FString::Printf(
        TEXT("ABP_AddSlotNode_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    const FString PackageName = FString::Printf(TEXT("/Game/PinWrightTests/%s"), *AssetName);
    const FString ObjectPath = FString::Printf(TEXT("%s.%s"), *PackageName, *AssetName);

    UAnimBlueprint* AnimBP = AGIRTestFixtures::CreateFreshAnimBlueprint(PackageName, Skeleton);
    TestNotNull(TEXT("transient AnimBlueprint created"), AnimBP);
    if (!AnimBP)
    {
        Skeleton->RemoveFromRoot();
        return true;
    }

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("blueprintPath"), ObjectPath);
    Payload->SetStringField(TEXT("slotName"), TEXT("LocoSlot"));
    Payload->SetStringField(TEXT("groupName"), TEXT("LocoGroup"));
    Payload->SetNumberField(TEXT("x"), 240.0);
    Payload->SetNumberField(TEXT("y"), 120.0);
    Payload->SetBoolField(TEXT("save"), false);

    FTestResponseCapture Capture;
    TestTrue(TEXT("animation.authoring.add_slot_node handler found"),
        InvokeHandlerWithCapture(TEXT("animation.authoring.add_slot_node"), Payload, Capture));

    // The AnimGraph/slot-node module headers ship on the test host, so the handler
    // succeeds there; if a build lacks them it returns an honest error instead of a
    // fake success, so gate the behavioral assertions on success.
    if (Capture.bSuccess && Capture.Result.IsValid())
    {
        // The response echoes the BARE slot name, not "LocoGroup.LocoSlot".
        FString EchoedSlot;
        TestTrue(TEXT("response carries slotName"),
            Capture.Result->TryGetStringField(TEXT("slotName"), EchoedSlot));
        TestEqual(TEXT("slotName is the bare name (no group prefix baked in)"),
            EchoedSlot, FString(TEXT("LocoSlot")));

        // groupName is honored via a skeleton-level slot-group registration.
        TestEqual(TEXT("skeleton maps the slot to the requested group"),
            Skeleton->GetSlotGroupName(FName(TEXT("LocoSlot"))), FName(TEXT("LocoGroup")));
    }
    else
    {
        TestTrue(TEXT("add_slot_node responded (module headers may be absent in this build)"),
            Capture.bWasCalled);
        // Pin the one honest-failure code the handler is allowed to take this branch
        // with (AnimationAuthoringHandler_AnimBlueprint.cpp, the #else of the
        // MCP_HAS_ANIM_STATE_MACHINE_GRAPH && MCP_HAS_SLOT_NODE guard). Without this
        // the branch asserts nothing: a handler that started failing for any other
        // reason — ANIM_BP_NOT_FOUND, GRAPH_NOT_FOUND, MISSING_SLOT_NAME — would land
        // here and the test would stay permanently green while covering nothing.
        TestEqual(
            TEXT("add_slot_node's only non-success outcome is the headers-absent error"),
            Capture.ErrorCode, FString(TEXT("ANIMGRAPH_MODULE_UNAVAILABLE")));
        AddWarning(TEXT("AnimGraph slot-node headers absent; skipping add_slot_node.BareSlotNameAndGroup."));
    }

    CleanupTestAsset(PackageName);
    Skeleton->RemoveFromRoot();
    return true;
}

// animation.authoring.add_control — removed; CRIR is the Control Rig graph authoring surface.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAuthoringAddControlRemovedHandlerAbsentTest,
    "PinWright.animation.authoring.add_control.RemovedHandlerAbsent",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FAuthoringAddControlRemovedHandlerAbsentTest::RunTest(const FString& Parameters)
{
    TestFalse(TEXT("animation.authoring.add_control is not registered"),
        IsRegistered(TEXT("animation.authoring.add_control")));
    return true;
}

// animation.authoring.add_rig_unit — removed; CRIR is the Control Rig graph authoring surface.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAuthoringAddRigUnitRemovedHandlerAbsentTest,
    "PinWright.animation.authoring.add_rig_unit.RemovedHandlerAbsent",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FAuthoringAddRigUnitRemovedHandlerAbsentTest::RunTest(const FString& Parameters)
{
    TestFalse(TEXT("animation.authoring.add_rig_unit is not registered"),
        IsRegistered(TEXT("animation.authoring.add_rig_unit")));
    return true;
}

// animation.authoring.connect_rig_elements — removed; CRIR is the Control Rig graph authoring surface.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAuthoringConnectRigElementsRemovedHandlerAbsentTest,
    "PinWright.animation.authoring.connect_rig_elements.RemovedHandlerAbsent",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FAuthoringConnectRigElementsRemovedHandlerAbsentTest::RunTest(const FString& Parameters)
{
    TestFalse(TEXT("animation.authoring.connect_rig_elements is not registered"),
        IsRegistered(TEXT("animation.authoring.connect_rig_elements")));
    return true;
}

// Regression for B-create-pose-library-noop-fake-success: animation.authoring.create_pose_library
// used to validate the skeleton then return success:true with a *synthesized* assetPath while
// creating no UPoseAsset — a silent no-op, so asset.exists on the returned path was false and
// get_animation_info returned ASSET_NOT_FOUND. The fix actually creates+registers a UPoseAsset
// bound to the skeleton. This test invokes the production handler with a real transient skeleton,
// asserts success, and confirms a real UPoseAsset loads at the returned assetPath and carries the
// skeleton. Reverting the fix (back to the fake-success envelope) makes StaticLoadObject return
// null and these assertions fail.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAuthoringCreatePoseLibraryCreatesRealAssetTest,
    "PinWright.animation.authoring.create_pose_library.CreatesRealAsset",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FAuthoringCreatePoseLibraryCreatesRealAssetTest::RunTest(const FString& Parameters)
{
#if MCP_TEST_HAS_POSEASSET
    FString SkeletonPath;
    USkeleton* Skeleton = NewLoadedTransientSkeleton(TEXT("SK_CreatePoseLib"), SkeletonPath);
    TestNotNull(TEXT("transient skeleton created"), Skeleton);
    if (!Skeleton)
    {
        return false;
    }

    const FString PoseName = FString::Printf(
        TEXT("PA_Test_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    const FString PoseFolder = TEXT("/Game/PinWrightTests");
    const FString PosePackagePath = FString::Printf(TEXT("%s/%s"), *PoseFolder, *PoseName);
    const FString PoseObjectPath = FString::Printf(TEXT("%s.%s"), *PosePackagePath, *PoseName);

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("name"), PoseName);
    Payload->SetStringField(TEXT("path"), PoseFolder);
    Payload->SetStringField(TEXT("skeletonPath"), SkeletonPath);
    Payload->SetBoolField(TEXT("save"), false);

    FTestResponseCapture Capture;
    TestTrue(TEXT("create_pose_library handler found"),
        InvokeHandlerWithCapture(TEXT("animation.authoring.create_pose_library"), Payload, Capture));
    TestTrue(TEXT("create_pose_library succeeds"), Capture.bWasCalled && Capture.bSuccess);

    // The returned assetPath must resolve to a real UPoseAsset (was a synthesized no-op path before).
    FString ReturnedPath = PoseObjectPath;
    if (Capture.Result.IsValid())
    {
        Capture.Result->TryGetStringField(TEXT("assetPath"), ReturnedPath);
    }

    UPoseAsset* PoseAsset = Cast<UPoseAsset>(
        StaticLoadObject(UPoseAsset::StaticClass(), nullptr, *ReturnedPath));
    TestNotNull(TEXT("created pose library loads as a real UPoseAsset"), PoseAsset);
    if (PoseAsset)
    {
        // The asset must be bound to the skeleton we passed — proves SetSkeleton ran, not just NewObject.
        TestEqual(TEXT("pose asset is bound to the requested skeleton"),
            PoseAsset->GetSkeleton(), Skeleton);
    }

    CleanupTestAsset(PosePackagePath);
    Skeleton->RemoveFromRoot();
    return true;
#else
    // Animation/PoseAsset.h absent in this build — assert only that the handler is registered.
    TestTrue(TEXT("animation.authoring.create_pose_library handler registered"),
        IsHandlerRegistered(TEXT("animation.authoring.create_pose_library")));
    return true;
#endif
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAuthoringGetAnimationInfoAnimSequenceParityFieldsTest,
    "PinWright.animation.authoring.get_animation_info.AnimSequenceParityFields",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FAuthoringGetAnimationInfoAnimSequenceParityFieldsTest::RunTest(const FString& Parameters)
{
    PinWrightAnimationTestFixtures::FRegisteredAnimationFixture Fixture;
    TestTrue(TEXT("registered animation fixture created"),
        Fixture.Create(TEXT("AS_GetAnimationInfoParity"), false, FFrameRate(24000, 1001)));
    UAnimSequence* Sequence = Fixture.Sequence;
    const FString ObjectPath = Fixture.SequenceObjectPath;
    TestNotNull(TEXT("registered sequence created"), Sequence);
    if (!Sequence)
    {
        return true;
    }
    Sequence->AdditiveAnimType = AAT_LocalSpaceBase;

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), ObjectPath);

    FTestResponseCapture Capture;
    TestTrue(TEXT("animation.authoring.get_animation_info handler found"),
        InvokeHandlerWithCapture(TEXT("animation.authoring.get_animation_info"), Payload, Capture));
    TestTrue(TEXT("get_animation_info succeeds"), Capture.bSuccess);
    TestTrue(TEXT("get_animation_info returned result"), Capture.Result.IsValid());

    const TSharedPtr<FJsonObject>* AnimationInfoPtr = nullptr;
    if (Capture.Result.IsValid())
    {
        TestTrue(TEXT("animationInfo object present"),
            Capture.Result->TryGetObjectField(TEXT("animationInfo"), AnimationInfoPtr));
    }

    if (AnimationInfoPtr && AnimationInfoPtr->IsValid())
    {
        const TSharedPtr<FJsonObject>& AnimationInfo = *AnimationInfoPtr;
        TestTrue(TEXT("legacy frameRate remains numeric"),
            AnimationInfo->HasTypedField<EJson::Number>(TEXT("frameRate")));
        TestTrue(TEXT("frameRateRational object present"),
            AnimationInfo->HasTypedField<EJson::Object>(TEXT("frameRateRational")));
        TestTrue(TEXT("additiveType string present"),
            AnimationInfo->HasTypedField<EJson::String>(TEXT("additiveType")));
        TestTrue(TEXT("rawTrackCount number present"),
            AnimationInfo->HasTypedField<EJson::Number>(TEXT("rawTrackCount")));
        TestTrue(TEXT("skeletonAssetPath string present"),
            AnimationInfo->HasTypedField<EJson::String>(TEXT("skeletonAssetPath")));

        const TSharedPtr<FJsonObject>* FrameRateRational = nullptr;
        if (AnimationInfo->TryGetObjectField(TEXT("frameRateRational"), FrameRateRational))
        {
            TestEqual(TEXT("frameRateRational numerator"), (*FrameRateRational)->GetNumberField(TEXT("numerator")), 24000.0);
            TestEqual(TEXT("frameRateRational denominator"), (*FrameRateRational)->GetNumberField(TEXT("denominator")), 1001.0);
        }
        TestEqual(TEXT("additiveType value"), AnimationInfo->GetStringField(TEXT("additiveType")), FString(TEXT("AAT_LocalSpaceBase")));
    }

    Sequence->RemoveFromRoot();
    return true;
}

// Regression for E-get-animation-info-thin-on-blend-space: the UBlendSpace branch of
// animation.authoring.get_animation_info must emit read-back parity with the asset.dump
// blend_space.json sidecar — axes[] (displayName/min/max/gridNum) and samples[]
// (animation/x/y/rateScale) — not just {assetType, skeletonPath, numSamples}. The handler
// delegates to the shared BlendSpaceDumpBuilder::BuildBlendSpaceJson; this test would fail
// if that delegation were reverted to the thin three-field shape. A transient (RF_Transient,
// so UObject::IsAsset()==false) UBlendSpace1D lets us configure the axis via the production
// GetBlendParametersForWrite helper and add a sample via the public skeleton-free AddSample
// overload without standing up a real skeleton/anim sequence.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAuthoringGetAnimationInfoBlendSpaceParityFieldsTest,
    "PinWright.animation.authoring.get_animation_info.BlendSpaceParityFields",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FAuthoringGetAnimationInfoBlendSpaceParityFieldsTest::RunTest(const FString& Parameters)
{
    FString ObjectPath;
    UBlendSpace1D* BlendSpace = NewLoadedTransientBlendSpace1D(TEXT("BS_GetAnimationInfoParity"), ObjectPath);
    TestNotNull(TEXT("transient blend space created"), BlendSpace);
    if (!BlendSpace)
    {
        return true;
    }

    // Configure the Speed axis through the same reflection helper the create handler uses.
    if (FBlendParameter* BlendParamsPtr = AnimationAuthoringHelpers::GetBlendParametersForWrite(BlendSpace))
    {
        FBlendParameter Param;
        Param.DisplayName = TEXT("Speed");
        Param.Min = 0.0f;
        Param.Max = 600.0f;
        Param.GridNum = 4;
        BlendParamsPtr[0] = Param;
    }
    // Skeleton-free AddSample (valid because the transient object is not an asset) places a
    // sample at x=250 so samples[] coordinate parity is assertable.
    const int32 SampleIndex = BlendSpace->AddSample(FVector(250.0, 0.0, 0.0));
    TestTrue(TEXT("blend sample added"), SampleIndex != INDEX_NONE);

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), ObjectPath);

    FTestResponseCapture Capture;
    TestTrue(TEXT("animation.authoring.get_animation_info handler found"),
        InvokeHandlerWithCapture(TEXT("animation.authoring.get_animation_info"), Payload, Capture));
    TestTrue(TEXT("get_animation_info succeeds"), Capture.bSuccess);
    TestTrue(TEXT("get_animation_info returned result"), Capture.Result.IsValid());

    const TSharedPtr<FJsonObject>* AnimationInfoPtr = nullptr;
    if (Capture.Result.IsValid())
    {
        TestTrue(TEXT("animationInfo object present"),
            Capture.Result->TryGetObjectField(TEXT("animationInfo"), AnimationInfoPtr));
    }

    if (AnimationInfoPtr && AnimationInfoPtr->IsValid())
    {
        const TSharedPtr<FJsonObject>& AnimationInfo = *AnimationInfoPtr;
        // Back-compat fields preserved.
        TestEqual(TEXT("assetType is BlendSpace1D"),
            AnimationInfo->GetStringField(TEXT("assetType")), FString(TEXT("BlendSpace1D")));
        TestEqual(TEXT("numSamples reflects added sample"),
            AnimationInfo->GetNumberField(TEXT("numSamples")), 1.0);

        // New parity arrays — the heart of the regression.
        const TArray<TSharedPtr<FJsonValue>>* AxesArr = nullptr;
        TestTrue(TEXT("axes array present"),
            AnimationInfo->TryGetArrayField(TEXT("axes"), AxesArr));
        if (AxesArr && AxesArr->Num() > 0 && (*AxesArr)[0].IsValid())
        {
            const TSharedPtr<FJsonObject> Axis0 = (*AxesArr)[0]->AsObject();
            if (Axis0.IsValid())
            {
                TestEqual(TEXT("axis displayName"), Axis0->GetStringField(TEXT("displayName")), FString(TEXT("Speed")));
                TestEqual(TEXT("axis max"), Axis0->GetNumberField(TEXT("max")), 600.0);
                TestEqual(TEXT("axis gridNum"), Axis0->GetNumberField(TEXT("gridNum")), 4.0);
            }
        }

        const TArray<TSharedPtr<FJsonValue>>* SamplesArr = nullptr;
        TestTrue(TEXT("samples array present"),
            AnimationInfo->TryGetArrayField(TEXT("samples"), SamplesArr));
        if (SamplesArr && SamplesArr->Num() > 0 && (*SamplesArr)[0].IsValid())
        {
            const TSharedPtr<FJsonObject> Sample0 = (*SamplesArr)[0]->AsObject();
            if (Sample0.IsValid())
            {
                TestEqual(TEXT("sample x coordinate"), Sample0->GetNumberField(TEXT("x")), 250.0);
            }
        }

        TestTrue(TEXT("interpolation array present"),
            AnimationInfo->HasTypedField<EJson::Array>(TEXT("interpolation")));
    }

    BlendSpace->RemoveFromRoot();
    return true;
}

// Regression for E-get-animation-info-thin-on-montage: the UAnimMontage branch of
// animation.authoring.get_animation_info must emit read-back parity with the asset.dump
// anim_montage.json sidecar — sections[] (sectionName/nextSectionName/startTime, the
// link_sections round-trip) and slots[] — not just {numSections, numSlots, duration}.
// The handler delegates to the shared AnimMontageDumpBuilder::BuildAnimMontageJson; this
// test would fail if that delegation were reverted to the counts-only shape. A transient
// (RF_Transient) UAnimMontage with two manually-linked composite sections (Idle->Walk)
// exercises production code without standing up a real skeleton.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAuthoringGetAnimationInfoMontageParityFieldsTest,
    "PinWright.animation.authoring.get_animation_info.MontageParityFields",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FAuthoringGetAnimationInfoMontageParityFieldsTest::RunTest(const FString& Parameters)
{
    FString ObjectPath;
    UAnimMontage* Montage = NewLoadedTransientAnimMontage(TEXT("AM_GetAnimationInfoParity"), ObjectPath);
    TestNotNull(TEXT("transient montage created"), Montage);
    if (!Montage)
    {
        return true;
    }

    // Two linked composite sections (Idle -> Walk) so the nextSectionName link map — the
    // exact thing link_sections authors and the ticket flags as unverifiable from
    // get_animation_info — is assertable through the live RPC.
    FCompositeSection Idle;
    Idle.SectionName = TEXT("Idle");
    Idle.NextSectionName = TEXT("Walk");
    Idle.SetTime(0.0f);
    FCompositeSection Walk;
    Walk.SectionName = TEXT("Walk");
    Walk.NextSectionName = NAME_None;
    Walk.SetTime(1.0f);
    Montage->CompositeSections.Add(Idle);
    Montage->CompositeSections.Add(Walk);

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), ObjectPath);

    FTestResponseCapture Capture;
    TestTrue(TEXT("animation.authoring.get_animation_info handler found"),
        InvokeHandlerWithCapture(TEXT("animation.authoring.get_animation_info"), Payload, Capture));
    TestTrue(TEXT("get_animation_info succeeds"), Capture.bSuccess);
    TestTrue(TEXT("get_animation_info returned result"), Capture.Result.IsValid());

    const TSharedPtr<FJsonObject>* AnimationInfoPtr = nullptr;
    if (Capture.Result.IsValid())
    {
        TestTrue(TEXT("animationInfo object present"),
            Capture.Result->TryGetObjectField(TEXT("animationInfo"), AnimationInfoPtr));
    }

    if (AnimationInfoPtr && AnimationInfoPtr->IsValid())
    {
        const TSharedPtr<FJsonObject>& AnimationInfo = *AnimationInfoPtr;
        // Back-compat fields preserved.
        TestEqual(TEXT("assetType is AnimMontage"),
            AnimationInfo->GetStringField(TEXT("assetType")), FString(TEXT("AnimMontage")));
        TestEqual(TEXT("numSections reflects added sections"),
            AnimationInfo->GetNumberField(TEXT("numSections")), 2.0);

        // New parity array — the heart of the regression: the section names and the
        // section->section nextSectionName link map.
        const TArray<TSharedPtr<FJsonValue>>* SectionsArr = nullptr;
        TestTrue(TEXT("sections array present"),
            AnimationInfo->TryGetArrayField(TEXT("sections"), SectionsArr));
        if (SectionsArr && SectionsArr->Num() >= 2 && (*SectionsArr)[0].IsValid())
        {
            const TSharedPtr<FJsonObject> Section0 = (*SectionsArr)[0]->AsObject();
            if (Section0.IsValid())
            {
                TestEqual(TEXT("section0 sectionName"),
                    Section0->GetStringField(TEXT("sectionName")), FString(TEXT("Idle")));
                TestEqual(TEXT("section0 nextSectionName link"),
                    Section0->GetStringField(TEXT("nextSectionName")), FString(TEXT("Walk")));
                TestTrue(TEXT("section0 startTime present"),
                    Section0->HasTypedField<EJson::Number>(TEXT("startTime")));
            }
        }

        TestTrue(TEXT("slots array present"),
            AnimationInfo->HasTypedField<EJson::Array>(TEXT("slots")));
    }

    Montage->RemoveFromRoot();
    return true;
}

// Regression for E-get-animation-info-thin-on-anim-blueprint: the UAnimBlueprint branch
// of animation.authoring.get_animation_info was the last branch of the parity family that
// only emitted {assetType, skeletonPath, parentClass} and never merged the AnimGraph dump,
// so the state machines / states / transitions that add_state_machine authors were
// unverifiable from the live RPC (only the asset.dump anim_graph.json sidecar carried them).
// The handler now delegates to AnimGraphDumpBuilder::BuildAnimGraphJson; this test builds a
// real graph (one state machine 'Locomotion' with Idle->WalkRun) and asserts the
// state_machines[] array — with the named machine, its states[], and its Idle->WalkRun
// transition — surfaces through get_animation_info. It would fail if the delegation were
// reverted to the metadata-only shape. Back-compat fields (assetType/parentClass) are kept.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAuthoringGetAnimationInfoAnimBlueprintParityFieldsTest,
    "PinWright.animation.authoring.get_animation_info.AnimBlueprintParityFields",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FAuthoringGetAnimationInfoAnimBlueprintParityFieldsTest::RunTest(const FString& Parameters)
{
#if MCP_TEST_HAS_STATE_MACHINE_GRAPH
    FString SkeletonPath;
    USkeleton* Skeleton = NewLoadedTransientSkeleton(TEXT("SK_GetAnimInfoABP"), SkeletonPath);
    TestNotNull(TEXT("transient skeleton created"), Skeleton);
    if (!Skeleton)
    {
        return false;
    }

    const FString AssetName = FString::Printf(
        TEXT("ABP_GetAnimInfoParity_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    const FString PackageName = FString::Printf(
        TEXT("/Game/PinWrightTests/%s"), *AssetName);
    const FString ObjectPath = FString::Printf(TEXT("%s.%s"), *PackageName, *AssetName);

    UAnimBlueprint* AnimBP = AGIRTestFixtures::CreateFreshAnimBlueprint(PackageName, Skeleton);
    TestNotNull(TEXT("transient AnimBlueprint created"), AnimBP);
    if (!AnimBP)
    {
        Skeleton->RemoveFromRoot();
        return false;
    }

    // Author a state machine 'Locomotion' (Idle->WalkRun) via the real handler so the
    // AnimGraph the parity merge must surface actually exists.
    {
        TSharedPtr<FJsonObject> SmPayload = MakeShared<FJsonObject>();
        SmPayload->SetStringField(TEXT("blueprintPath"), ObjectPath);
        SmPayload->SetStringField(TEXT("machineName"), TEXT("Locomotion"));

        TArray<TSharedPtr<FJsonValue>> States;
        {
            TSharedPtr<FJsonObject> Idle = MakeShared<FJsonObject>();
            Idle->SetStringField(TEXT("name"), TEXT("Idle"));
            Idle->SetBoolField(TEXT("isEntry"), true);
            States.Add(MakeShared<FJsonValueObject>(Idle));

            TSharedPtr<FJsonObject> Walk = MakeShared<FJsonObject>();
            Walk->SetStringField(TEXT("name"), TEXT("WalkRun"));
            States.Add(MakeShared<FJsonValueObject>(Walk));
        }
        SmPayload->SetArrayField(TEXT("states"), States);

        TArray<TSharedPtr<FJsonValue>> Transitions;
        {
            TSharedPtr<FJsonObject> Trans = MakeShared<FJsonObject>();
            Trans->SetStringField(TEXT("sourceState"), TEXT("Idle"));
            Trans->SetStringField(TEXT("targetState"), TEXT("WalkRun"));
            Transitions.Add(MakeShared<FJsonValueObject>(Trans));
        }
        SmPayload->SetArrayField(TEXT("transitions"), Transitions);

        FTestResponseCapture SmCapture;
        TestTrue(TEXT("animation.create_state_machine handler found"),
            InvokeHandlerWithCapture(TEXT("animation.create_state_machine"), SmPayload, SmCapture));
        TestTrue(TEXT("create_state_machine succeeds"), SmCapture.bWasCalled && SmCapture.bSuccess);
    }

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), ObjectPath);

    FTestResponseCapture Capture;
    TestTrue(TEXT("animation.authoring.get_animation_info handler found"),
        InvokeHandlerWithCapture(TEXT("animation.authoring.get_animation_info"), Payload, Capture));
    TestTrue(TEXT("get_animation_info succeeds"), Capture.bSuccess);
    TestTrue(TEXT("get_animation_info returned result"), Capture.Result.IsValid());

    const TSharedPtr<FJsonObject>* AnimationInfoPtr = nullptr;
    if (Capture.Result.IsValid())
    {
        TestTrue(TEXT("animationInfo object present"),
            Capture.Result->TryGetObjectField(TEXT("animationInfo"), AnimationInfoPtr));
    }

    if (AnimationInfoPtr && AnimationInfoPtr->IsValid())
    {
        const TSharedPtr<FJsonObject>& AnimationInfo = *AnimationInfoPtr;
        // Back-compat fields preserved (assetType/parentClass take precedence under the merge).
        TestEqual(TEXT("assetType is AnimBlueprint"),
            AnimationInfo->GetStringField(TEXT("assetType")), FString(TEXT("AnimBlueprint")));
        TestTrue(TEXT("parentClass present"),
            AnimationInfo->HasTypedField<EJson::String>(TEXT("parentClass")));

        // New parity array — the heart of the regression: the authored state machine, its
        // states, and the Idle->WalkRun transition the dump sidecar carried must now surface
        // through the live RPC.
        const TArray<TSharedPtr<FJsonValue>>* MachinesArr = nullptr;
        TestTrue(TEXT("state_machines array present"),
            AnimationInfo->TryGetArrayField(TEXT("state_machines"), MachinesArr));
        if (MachinesArr && MachinesArr->Num() > 0 && (*MachinesArr)[0].IsValid())
        {
            const TSharedPtr<FJsonObject> Machine0 = (*MachinesArr)[0]->AsObject();
            if (Machine0.IsValid())
            {
                TestEqual(TEXT("state machine name 'Locomotion'"),
                    Machine0->GetStringField(TEXT("name")), FString(TEXT("Locomotion")));

                const TArray<TSharedPtr<FJsonValue>>* StatesArr = nullptr;
                TestTrue(TEXT("state machine states[] present"),
                    Machine0->TryGetArrayField(TEXT("states"), StatesArr));

                const TArray<TSharedPtr<FJsonValue>>* TransArr = nullptr;
                TestTrue(TEXT("state machine transitions[] present"),
                    Machine0->TryGetArrayField(TEXT("transitions"), TransArr));
                bool bFoundIdleToWalk = false;
                if (TransArr)
                {
                    for (const TSharedPtr<FJsonValue>& TV : *TransArr)
                    {
                        const TSharedPtr<FJsonObject> TObj = TV.IsValid() ? TV->AsObject() : nullptr;
                        if (TObj.IsValid() &&
                            TObj->GetStringField(TEXT("from")) == TEXT("Idle") &&
                            TObj->GetStringField(TEXT("to")) == TEXT("WalkRun"))
                        {
                            bFoundIdleToWalk = true;
                            break;
                        }
                    }
                }
                TestTrue(TEXT("Idle->WalkRun transition surfaces through get_animation_info"),
                    bFoundIdleToWalk);
            }
        }
    }

    CleanupTestAsset(PackageName);
    Skeleton->RemoveFromRoot();
    return true;
#else
    // AnimGraph state-machine module headers absent in this build — assert only that the
    // handler is registered so the test still links and passes.
    TestTrue(TEXT("animation.authoring.get_animation_info handler registered"),
        IsHandlerRegistered(TEXT("animation.authoring.get_animation_info")));
    return true;
#endif
}

// ============================================================================
// MorphTargetHandler.cpp — 6 handlers
// ============================================================================

// skeleton.create_morph_target — regression for ticket
// B-create-morph-target-empty-not-persisted: creating an empty (delta-less) morph
// must NOT fake-success or reach USkeletalMesh::RegisterMorphTarget, whose UE 5.8
// precondition ensures when HasValidData() is false. The handler rejects the zero-
// delta target first with MORPH_NOT_PERSISTED. This exercises the production
// handler against a real transient USkeletalMesh loaded by path.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMorphCreateEmptyNotPersistedTest,
    "PinWright.skeleton.create_morph_target.EmptyMorphNotFakeSuccess",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FMorphCreateEmptyNotPersistedTest::RunTest(const FString& Parameters)
{
    // Build a transient skeletal mesh the handler can StaticLoadObject by path.
    const FString AssetName = FString::Printf(TEXT("SK_MorphEmptyTest_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    const FString PackageName = FString::Printf(
        TEXT("/Game/PinWrightTests/%s"), *AssetName);
    UPackage* Package = CreatePackage(*PackageName);
    USkeletalMesh* Mesh = NewObject<USkeletalMesh>(
        Package, *AssetName, RF_Public | RF_Standalone | RF_Transient);
    if (!TestNotNull(TEXT("transient skeletal mesh created"), Mesh))
    {
        return true;
    }

    const FString MeshObjectPath = FString::Printf(TEXT("%s.%s"), *PackageName, *AssetName);
    TestEqual(TEXT("mesh starts with no morph targets"), Mesh->GetMorphTargets().Num(), 0);

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("skeletalMeshPath"), MeshObjectPath);
    Payload->SetStringField(TEXT("morphTargetName"), TEXT("Jaw_Open"));

    FMcpOutputCapture LogCapture;
    GLog->AddOutputDevice(&LogCapture);

    FTestResponseCapture Capture;
    const bool bFound = InvokeHandlerWithCapture(
        TEXT("skeleton.create_morph_target"), Payload, Capture);

    GLog->Flush();
    GLog->RemoveOutputDevice(&LogCapture);

    TestTrue(TEXT("skeleton.create_morph_target handler found"), bFound);

    // The empty morph cannot persist, so the handler must report failure — never the
    // old fake-success {morphTargetName, morphTargetCount:0}.
    TestFalse(TEXT("empty-morph create is NOT reported as success"), Capture.bSuccess);
    TestEqual(TEXT("error code is MORPH_NOT_PERSISTED"),
        Capture.ErrorCode, FString(ErrorCodes::ERR_MORPH_NOT_PERSISTED));

    bool bMorphValidityEnsureLogged = false;
    for (const FString& Line : LogCapture.Lines)
    {
        if (Line.Contains(TEXT("MorphTarget->HasValidData()")) ||
            Line.Contains(TEXT("RegisterMorphTarget: Jaw_Open has empty data")))
        {
            bMorphValidityEnsureLogged = true;
            break;
        }
    }
    TestFalse(TEXT("Empty morph was rejected without an engine ensure"),
        bMorphValidityEnsureLogged);

    // The handler refused before registration, so the mesh still has no morph targets.
    TestEqual(TEXT("no morph target persisted on the mesh"),
        Mesh->GetMorphTargets().Num(), 0);

    return true;
}

// ============================================================================
// PhysicsAssetHandler.cpp — 11 handlers
// ============================================================================

// skeleton.create_physics_asset — bare-skeleton path (ticket
// F-skeleton-no-mesh-for-physics-asset). A USkeleton authored purely via the
// skeleton.* family has no bound SkeletalMesh and no preview mesh, so before the
// fix create_physics_asset returned [MESH_NOT_FOUND] for it and the "author a
// skeleton, then create a physics asset for it" workflow was unreachable end to
// end. The handler now falls back to building one capsule body per bone directly
// from the skeleton's reference-pose bone span. This drives the real production
// authoring chain (create_skeleton -> add_bone -> create_physics_asset) through
// the registration table; if the bare-skeleton fallback is reverted the
// create_physics_asset call returns MESH_NOT_FOUND and the success / bodyCount
// assertions below fail.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPhysicsCreateAssetFromBareSkeletonTest,
    "PinWright.skeleton.create_physics_asset.FromBareSkeleton",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FPhysicsCreateAssetFromBareSkeletonTest::RunTest(const FString& Parameters)
{
    const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString SkeletonPath = FString::Printf(
        TEXT("/Game/PinWrightTests/SK_BarePhys_%s"), *Suffix);
    const FString PhysicsAssetPath = FString::Printf(
        TEXT("/Game/PinWrightTests/PA_BarePhys_%s"), *Suffix);

    // 1) Author a bare meshless skeleton with a measurable bone chain via the
    //    production handlers (repro steps 1-2).
    {
        TSharedPtr<FJsonObject> CreatePayload = MakeShared<FJsonObject>();
        CreatePayload->SetStringField(TEXT("path"), SkeletonPath);
        CreatePayload->SetStringField(TEXT("rootBoneName"), TEXT("root"));
        FTestResponseCapture CreateCapture;
        TestTrue(TEXT("skeleton.create_skeleton handler found"),
            InvokeHandlerWithCapture(TEXT("skeleton.create_skeleton"), CreatePayload, CreateCapture));
        TestTrue(TEXT("create_skeleton succeeds"), CreateCapture.bWasCalled && CreateCapture.bSuccess);
    }

    // Add a child bone offset 30cm down +X so the root has a non-degenerate span.
    {
        TSharedPtr<FJsonObject> BonePayload = MakeShared<FJsonObject>();
        BonePayload->SetStringField(TEXT("skeletonPath"), SkeletonPath);
        BonePayload->SetStringField(TEXT("boneName"), TEXT("spine_01"));
        BonePayload->SetStringField(TEXT("parentBone"), TEXT("root"));
        TSharedPtr<FJsonObject> Loc = MakeShared<FJsonObject>();
        Loc->SetNumberField(TEXT("x"), 30.0);
        Loc->SetNumberField(TEXT("y"), 0.0);
        Loc->SetNumberField(TEXT("z"), 0.0);
        BonePayload->SetObjectField(TEXT("location"), Loc);
        FTestResponseCapture BoneCapture;
        TestTrue(TEXT("skeleton.add_bone handler found"),
            InvokeHandlerWithCapture(TEXT("skeleton.add_bone"), BonePayload, BoneCapture));
        TestTrue(TEXT("add_bone succeeds"), BoneCapture.bWasCalled && BoneCapture.bSuccess);
    }

    // 2) Create a physics asset directly from the bare skeleton path (repro
    //    step 3 — pre-fix this was [MESH_NOT_FOUND]).
    FTestResponseCapture PhysCapture;
    TSharedPtr<FJsonObject> PhysPayload = MakeShared<FJsonObject>();
    PhysPayload->SetStringField(TEXT("skeletonPath"), SkeletonPath);
    PhysPayload->SetStringField(TEXT("outputPath"), PhysicsAssetPath);
    const bool bFound = InvokeHandlerWithCapture(
        TEXT("skeleton.create_physics_asset"), PhysPayload, PhysCapture);
    TestTrue(TEXT("skeleton.create_physics_asset handler found"), bFound);
    TestTrue(TEXT("create_physics_asset succeeds for a bare skeleton (no MESH_NOT_FOUND)"),
        PhysCapture.bWasCalled && PhysCapture.bSuccess);
    TestNotEqual(TEXT("error is not the pre-fix MESH_NOT_FOUND"),
        PhysCapture.ErrorCode, FString(TEXT("MESH_NOT_FOUND")));

    // 3) The created asset has at least one bone-derived capsule body.
    if (PhysCapture.bSuccess && PhysCapture.Result.IsValid())
    {
        double BodyCount = 0.0;
        TestTrue(TEXT("result reports bodyCount"),
            PhysCapture.Result->TryGetNumberField(TEXT("bodyCount"), BodyCount));
        TestTrue(TEXT("at least one physics body generated from bones"), BodyCount >= 1.0);

        UPhysicsAsset* PhysAsset = Cast<UPhysicsAsset>(
            StaticLoadObject(UPhysicsAsset::StaticClass(), nullptr, *PhysicsAssetPath));
        TestNotNull(TEXT("created physics asset loads"), PhysAsset);
        if (PhysAsset)
        {
            TestTrue(TEXT("physics asset has bone-keyed body setups"),
                PhysAsset->SkeletalBodySetups.Num() >= 1);
            bool bHasCapsule = false;
            for (USkeletalBodySetup* BodySetup : PhysAsset->SkeletalBodySetups)
            {
                if (BodySetup && BodySetup->AggGeom.SphylElems.Num() > 0)
                {
                    bHasCapsule = true;
                    break;
                }
            }
            TestTrue(TEXT("a generated body carries a capsule primitive"), bHasCapsule);
        }
    }

    CleanupTestAsset(PhysicsAssetPath);
    CleanupTestAsset(SkeletonPath);
    return true;
}

// Regression test for E-get-physics-asset-info-doc-fields-mismatch.
//
// skeleton.get_physics_asset_info's registered summary promises a lightweight
// "summary" carrying "total primitive count" and "the bound SkeletalMesh path",
// and calls itself the read-only counterpart to skeleton.list_physics_bodies.
// Pre-fix the result carried ONLY {physicsAssetPath, name, numBodies,
// numConstraints} plus the FULL bodies[]+constraints[] arrays — there was no
// total-primitive scalar and no bound-mesh path, and the "summary" was a
// superset (larger than list_physics_bodies) that overflowed the inline spill
// threshold. The fix adds a numPrimitives scalar and a skeletalMeshPath field,
// and gates the full arrays behind includeBodies (default off).
//
// This builds a real UPhysicsAsset through the production authoring chain
// (create_skeleton -> add_bone -> create_physics_asset, which yields one capsule
// per bone), adds a deterministic extra sphere primitive so the total is known,
// binds a preview SkeletalMesh, then dispatches get_physics_asset_info through
// the real registered handler and asserts: numPrimitives == the actual summed
// shape count; skeletalMeshPath == the bound mesh path; the bodies/constraints
// arrays are ABSENT by default and PRESENT under includeBodies=true.
//
// Counterfactual: revert the fix (drop numPrimitives/skeletalMeshPath and always
// dump the arrays) and the scalar/path assertions and the default-off gating
// assertion fail.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPhysicsGetAssetInfoSummaryFieldsTest,
    "PinWright.skeleton.get_physics_asset_info.SummaryFields",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FPhysicsGetAssetInfoSummaryFieldsTest::RunTest(const FString& Parameters)
{
    const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString SkeletonPath = FString::Printf(
        TEXT("/Game/PinWrightTests/SK_PhysInfo_%s"), *Suffix);
    const FString PhysicsAssetPath = FString::Printf(
        TEXT("/Game/PinWrightTests/PA_PhysInfo_%s"), *Suffix);

    // 1) Author a bare skeleton with a measurable bone chain (root + one child).
    {
        TSharedPtr<FJsonObject> CreatePayload = MakeShared<FJsonObject>();
        CreatePayload->SetStringField(TEXT("path"), SkeletonPath);
        CreatePayload->SetStringField(TEXT("rootBoneName"), TEXT("root"));
        FTestResponseCapture CreateCapture;
        TestTrue(TEXT("skeleton.create_skeleton handler found"),
            InvokeHandlerWithCapture(TEXT("skeleton.create_skeleton"), CreatePayload, CreateCapture));
        TestTrue(TEXT("create_skeleton succeeds"), CreateCapture.bWasCalled && CreateCapture.bSuccess);
    }
    {
        TSharedPtr<FJsonObject> BonePayload = MakeShared<FJsonObject>();
        BonePayload->SetStringField(TEXT("skeletonPath"), SkeletonPath);
        BonePayload->SetStringField(TEXT("boneName"), TEXT("spine_01"));
        BonePayload->SetStringField(TEXT("parentBone"), TEXT("root"));
        TSharedPtr<FJsonObject> Loc = MakeShared<FJsonObject>();
        Loc->SetNumberField(TEXT("x"), 30.0);
        Loc->SetNumberField(TEXT("y"), 0.0);
        Loc->SetNumberField(TEXT("z"), 0.0);
        BonePayload->SetObjectField(TEXT("location"), Loc);
        FTestResponseCapture BoneCapture;
        TestTrue(TEXT("skeleton.add_bone handler found"),
            InvokeHandlerWithCapture(TEXT("skeleton.add_bone"), BonePayload, BoneCapture));
        TestTrue(TEXT("add_bone succeeds"), BoneCapture.bWasCalled && BoneCapture.bSuccess);
    }

    // 2) Create a physics asset directly from the bare skeleton (one capsule per bone).
    {
        FTestResponseCapture PhysCapture;
        TSharedPtr<FJsonObject> PhysPayload = MakeShared<FJsonObject>();
        PhysPayload->SetStringField(TEXT("skeletonPath"), SkeletonPath);
        PhysPayload->SetStringField(TEXT("outputPath"), PhysicsAssetPath);
        TestTrue(TEXT("skeleton.create_physics_asset handler found"),
            InvokeHandlerWithCapture(TEXT("skeleton.create_physics_asset"), PhysPayload, PhysCapture));
        TestTrue(TEXT("create_physics_asset succeeds"),
            PhysCapture.bWasCalled && PhysCapture.bSuccess);
    }

    UPhysicsAsset* PhysAsset = Cast<UPhysicsAsset>(
        StaticLoadObject(UPhysicsAsset::StaticClass(), nullptr, *PhysicsAssetPath));
    if (!TestNotNull(TEXT("created physics asset loads"), PhysAsset))
    {
        CleanupTestAsset(PhysicsAssetPath);
        CleanupTestAsset(SkeletonPath);
        return true;
    }
    if (!TestTrue(TEXT("physics asset has at least one body"),
            PhysAsset->SkeletalBodySetups.Num() >= 1) || !PhysAsset->SkeletalBodySetups[0])
    {
        CleanupTestAsset(PhysicsAssetPath);
        CleanupTestAsset(SkeletonPath);
        return true;
    }

    // Add a deterministic extra sphere primitive so the total primitive count is
    // a known, asserted value (the chain produces capsules only).
    PhysAsset->SkeletalBodySetups[0]->AggGeom.SphereElems.Add(FKSphereElem(2.0f));

    // Compute the expected total exactly the way the handler now sums it.
    int32 ExpectedPrimitives = 0;
    for (USkeletalBodySetup* BodySetup : PhysAsset->SkeletalBodySetups)
    {
        if (BodySetup)
        {
            ExpectedPrimitives += BodySetup->AggGeom.SphereElems.Num()
                + BodySetup->AggGeom.BoxElems.Num()
                + BodySetup->AggGeom.SphylElems.Num()
                + BodySetup->AggGeom.ConvexElems.Num();
        }
    }
    TestTrue(TEXT("test fixture has at least one primitive"), ExpectedPrimitives >= 1);

    // Bind a bare in-memory preview SkeletalMesh so the bound-mesh path is
    // reportable. Assign PreviewSkeletalMesh directly rather than via
    // SetPreviewMesh(): SetPreviewMesh validates that every body's bone exists in
    // the mesh's ref skeleton and pops a modal FMessageDialog on a mismatch
    // (a bare mesh has an empty ref skeleton) — which would hang under -unattended.
    // GetPreviewMesh() (the handler's source for skeletalMeshPath) reads this field.
    USkeletalMesh* PreviewMesh = NewObject<USkeletalMesh>(
        PhysAsset->GetOutermost(), TEXT("SK_PhysInfoPreview"), RF_Transient);
    TestNotNull(TEXT("preview skeletal mesh created"), PreviewMesh);
    PhysAsset->PreviewSkeletalMesh = PreviewMesh;

    // 3) Default call: the lightweight summary. Assert the doc-promised scalar
    //    and mesh path are present, and the heavy arrays are gated OFF.
    {
        FTestResponseCapture Capture;
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("physicsAssetPath"), PhysicsAssetPath);
        TestTrue(TEXT("skeleton.get_physics_asset_info handler found"),
            InvokeHandlerWithCapture(TEXT("skeleton.get_physics_asset_info"), Payload, Capture));
        TestTrue(TEXT("get_physics_asset_info succeeds"), Capture.bWasCalled && Capture.bSuccess);

        if (Capture.bSuccess && Capture.Result.IsValid())
        {
            // numPrimitives — the doc's "total primitive count".
            double NumPrimitives = -1.0;
            TestTrue(TEXT("result carries numPrimitives (the doc's 'total primitive count')"),
                Capture.Result->TryGetNumberField(TEXT("numPrimitives"), NumPrimitives));
            TestEqual(TEXT("numPrimitives equals the summed shape count across all bodies"),
                static_cast<int32>(NumPrimitives), ExpectedPrimitives);

            // skeletalMeshPath — the doc's "bound SkeletalMesh path".
            FString MeshPath;
            TestTrue(TEXT("result carries skeletalMeshPath (the doc's 'bound SkeletalMesh path')"),
                Capture.Result->TryGetStringField(TEXT("skeletalMeshPath"), MeshPath));
            TestEqual(TEXT("skeletalMeshPath is the bound preview mesh's path"),
                MeshPath, PreviewMesh->GetPathName());

            // The "summary" must stay a summary by default: the full per-body /
            // per-constraint arrays are gated behind includeBodies.
            const TArray<TSharedPtr<FJsonValue>>* DummyArr = nullptr;
            TestFalse(TEXT("bodies[] is absent in the default summary"),
                Capture.Result->TryGetArrayField(TEXT("bodies"), DummyArr));
            TestFalse(TEXT("constraints[] is absent in the default summary"),
                Capture.Result->TryGetArrayField(TEXT("constraints"), DummyArr));
        }
    }

    // 4) includeBodies=true restores the full enumeration (opt-in verbose mode).
    {
        FTestResponseCapture Capture;
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("physicsAssetPath"), PhysicsAssetPath);
        Payload->SetBoolField(TEXT("includeBodies"), true);
        TestTrue(TEXT("get_physics_asset_info (verbose) handler found"),
            InvokeHandlerWithCapture(TEXT("skeleton.get_physics_asset_info"), Payload, Capture));
        TestTrue(TEXT("get_physics_asset_info (verbose) succeeds"),
            Capture.bWasCalled && Capture.bSuccess);

        if (Capture.bSuccess && Capture.Result.IsValid())
        {
            const TArray<TSharedPtr<FJsonValue>>* Bodies = nullptr;
            TestTrue(TEXT("includeBodies=true emits the bodies[] array"),
                Capture.Result->TryGetArrayField(TEXT("bodies"), Bodies));
            if (Bodies)
            {
                TestEqual(TEXT("bodies[] length matches numBodies"),
                    Bodies->Num(), PhysAsset->SkeletalBodySetups.Num());
            }
        }
    }

    PhysAsset->PreviewSkeletalMesh = nullptr;
    CleanupTestAsset(PhysicsAssetPath);
    CleanupTestAsset(SkeletonPath);
    return true;
}

// skeleton.list_physics_bodies — all params optional; empty payload triggers error path.
// ValidParamsNoCrash: exercises the empty-path error branch without crashing.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPhysicsListBodiesValidNoCrashTest,
    "PinWright.skeleton.list_physics_bodies.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FPhysicsListBodiesValidNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    TestTrue(TEXT("skeleton.list_physics_bodies handler found"),
        InvokeHandler(TEXT("skeleton.list_physics_bodies"), Payload));
    return true;
}

// Fixed-behavior regression for the configure_physics_body B9+B7 defect: the
// requested mass was read into a dead local while MassScale was hardcoded to its
// default and bOverrideMass was flipped WITHOUT writing MassInKgOverride — so the
// body's mass silently snapped to the engine default override of 100 kg. The fix
// routes through FBodyInstance::SetMassOverride, which writes both fields.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPhysicsConfigureBodyMassOverrideTest,
    "PinWright.skeleton.configure_physics_body.MassOverrideApplied",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FPhysicsConfigureBodyMassOverrideTest::RunTest(const FString& Parameters)
{
    const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString SkeletonPath = FString::Printf(
        TEXT("/Game/PinWrightTests/SK_ConfigBodyMass_%s"), *Suffix);
    const FString PhysicsAssetPath = FString::Printf(
        TEXT("/Game/PinWrightTests/PA_ConfigBodyMass_%s"), *Suffix);

    // Author a bare skeleton (root + one child) and a physics asset from it.
    {
        TSharedPtr<FJsonObject> CreatePayload = MakeShared<FJsonObject>();
        CreatePayload->SetStringField(TEXT("path"), SkeletonPath);
        CreatePayload->SetStringField(TEXT("rootBoneName"), TEXT("root"));
        FTestResponseCapture CreateCapture;
        TestTrue(TEXT("skeleton.create_skeleton handler found"),
            InvokeHandlerWithCapture(TEXT("skeleton.create_skeleton"), CreatePayload, CreateCapture));
        TestTrue(TEXT("create_skeleton succeeds"), CreateCapture.bWasCalled && CreateCapture.bSuccess);
    }
    {
        TSharedPtr<FJsonObject> BonePayload = MakeShared<FJsonObject>();
        BonePayload->SetStringField(TEXT("skeletonPath"), SkeletonPath);
        BonePayload->SetStringField(TEXT("boneName"), TEXT("spine_01"));
        BonePayload->SetStringField(TEXT("parentBone"), TEXT("root"));
        TSharedPtr<FJsonObject> Loc = MakeShared<FJsonObject>();
        Loc->SetNumberField(TEXT("x"), 30.0);
        Loc->SetNumberField(TEXT("y"), 0.0);
        Loc->SetNumberField(TEXT("z"), 0.0);
        BonePayload->SetObjectField(TEXT("location"), Loc);
        FTestResponseCapture BoneCapture;
        TestTrue(TEXT("skeleton.add_bone handler found"),
            InvokeHandlerWithCapture(TEXT("skeleton.add_bone"), BonePayload, BoneCapture));
        TestTrue(TEXT("add_bone succeeds"), BoneCapture.bWasCalled && BoneCapture.bSuccess);
    }
    {
        FTestResponseCapture PhysCapture;
        TSharedPtr<FJsonObject> PhysPayload = MakeShared<FJsonObject>();
        PhysPayload->SetStringField(TEXT("skeletonPath"), SkeletonPath);
        PhysPayload->SetStringField(TEXT("outputPath"), PhysicsAssetPath);
        TestTrue(TEXT("skeleton.create_physics_asset handler found"),
            InvokeHandlerWithCapture(TEXT("skeleton.create_physics_asset"), PhysPayload, PhysCapture));
        TestTrue(TEXT("create_physics_asset succeeds"), PhysCapture.bWasCalled && PhysCapture.bSuccess);
    }

    UPhysicsAsset* PhysAsset = Cast<UPhysicsAsset>(
        StaticLoadObject(UPhysicsAsset::StaticClass(), nullptr, *PhysicsAssetPath));
    if (!TestNotNull(TEXT("created physics asset loads"), PhysAsset) ||
        !TestTrue(TEXT("physics asset has at least one body"),
            PhysAsset->SkeletalBodySetups.Num() >= 1) ||
        !PhysAsset->SkeletalBodySetups[0])
    {
        CleanupTestAsset(PhysicsAssetPath);
        CleanupTestAsset(SkeletonPath);
        return true;
    }

    USkeletalBodySetup* BodySetup = PhysAsset->SkeletalBodySetups[0];
    const FString BodyBoneName = BodySetup->BoneName.ToString();

    // Configure the body's mass through the handler.
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("physicsAssetPath"), PhysicsAssetPath);
        Payload->SetStringField(TEXT("boneName"), BodyBoneName);
        Payload->SetNumberField(TEXT("mass"), 42.0);
        FTestResponseCapture Capture;
        TestTrue(TEXT("skeleton.configure_physics_body handler found"),
            InvokeHandlerWithCapture(TEXT("skeleton.configure_physics_body"), Payload, Capture));
        TestTrue(TEXT("configure_physics_body succeeds"), Capture.bWasCalled && Capture.bSuccess);
    }

    // The requested mass must actually take effect: SetMassOverride writes BOTH
    // bOverrideMass and MassInKgOverride (the bug left the mass at the 100 kg default).
    TestTrue(TEXT("bOverrideMass is enabled"), BodySetup->DefaultInstance.bOverrideMass != 0);
    TestEqual(TEXT("MassInKgOverride equals the requested mass"),
        BodySetup->DefaultInstance.GetMassOverride(), 42.0f);

    CleanupTestAsset(PhysicsAssetPath);
    CleanupTestAsset(SkeletonPath);
    return true;
}

// skeleton.get_physics_asset_info — all params optional; empty payload exercises the not-found path.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPhysicsGetAssetInfoValidNoCrashTest,
    "PinWright.skeleton.get_physics_asset_info.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FPhysicsGetAssetInfoValidNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    TestTrue(TEXT("skeleton.get_physics_asset_info handler found"),
        InvokeHandler(TEXT("skeleton.get_physics_asset_info"), Payload));
    return true;
}

// ============================================================================
// SkeletalMeshHandler.cpp — 8 handlers
// ============================================================================

// Regression for B-skeleton-copy-weights-noop-zero-fill: skeleton.copy_weights used to
// FMemory::Memzero every target vertex's FRawSkinWeight and report success — a destructive
// zero-fill that read nothing from the source. This exercises the production transfer math
// (SkinWeightTransferUtils::CopyClosestVertexWeights, the exact function the handler calls)
// and asserts each target vertex inherits its nearest source vertex's influences. Under the
// old zero-fill code every assertion below would fail (all-zero bones/weights).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSkelMeshCopyWeightsClosestVertexTransferTest,
    "PinWright.skeleton.copy_weights.ClosestVertexTransfer",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSkelMeshCopyWeightsClosestVertexTransferTest::RunTest(const FString& Parameters)
{
    // Two source vertices with distinct, non-zero influences at known positions.
    TArray<FSoftSkinVertex> SourceVertices;
    {
        FSoftSkinVertex A;
        FMemory::Memzero(&A, sizeof(FSoftSkinVertex));
        A.Position = FVector3f(0.0f, 0.0f, 0.0f);
        A.InfluenceBones[0] = 3;
        A.InfluenceWeights[0] = 65535;  // full weight on bone 3
        SourceVertices.Add(A);

        FSoftSkinVertex B;
        FMemory::Memzero(&B, sizeof(FSoftSkinVertex));
        B.Position = FVector3f(100.0f, 0.0f, 0.0f);
        B.InfluenceBones[0] = 7;
        B.InfluenceWeights[0] = 40000;
        B.InfluenceBones[1] = 9;
        B.InfluenceWeights[1] = 25535;
        SourceVertices.Add(B);
    }

    // Target vertices positioned near each source vertex (remeshed-asset stand-in):
    // T0 is close to source A, T1 is close to source B.
    TArray<FSoftSkinVertex> TargetVertices;
    {
        FSoftSkinVertex T0;
        FMemory::Memzero(&T0, sizeof(FSoftSkinVertex));
        T0.Position = FVector3f(5.0f, 1.0f, 0.0f);   // nearest = A
        TargetVertices.Add(T0);

        FSoftSkinVertex T1;
        FMemory::Memzero(&T1, sizeof(FSoftSkinVertex));
        T1.Position = FVector3f(98.0f, -2.0f, 0.0f); // nearest = B
        TargetVertices.Add(T1);
    }

    TArray<FRawSkinWeight> OutWeights;
    const int32 Copied = SkinWeightTransferUtils::CopyClosestVertexWeights(
        SourceVertices, TargetVertices, OutWeights);

    TestEqual(TEXT("all target vertices written"), Copied, TargetVertices.Num());
    TestEqual(TEXT("one FRawSkinWeight per target vertex"), OutWeights.Num(), TargetVertices.Num());
    if (OutWeights.Num() != 2)
    {
        return false;
    }

    // T0 inherited source A's influence (bone 3, full weight) — NOT a zero-fill.
    TestEqual(TEXT("T0 bone matches nearest source A"),
        static_cast<int32>(OutWeights[0].InfluenceBones[0]), 3);
    TestEqual(TEXT("T0 weight matches nearest source A"),
        static_cast<int32>(OutWeights[0].InfluenceWeights[0]), 65535);

    // T1 inherited source B's two influences.
    TestEqual(TEXT("T1 first bone matches nearest source B"),
        static_cast<int32>(OutWeights[1].InfluenceBones[0]), 7);
    TestEqual(TEXT("T1 first weight matches nearest source B"),
        static_cast<int32>(OutWeights[1].InfluenceWeights[0]), 40000);
    TestEqual(TEXT("T1 second bone matches nearest source B"),
        static_cast<int32>(OutWeights[1].InfluenceBones[1]), 9);
    TestEqual(TEXT("T1 second weight matches nearest source B"),
        static_cast<int32>(OutWeights[1].InfluenceWeights[1]), 25535);

    // Guard the no-op masking specifically: at least one copied weight is non-zero,
    // which the old Memzero zero-fill could never produce.
    const bool bAnyNonZeroWeight =
        OutWeights[0].InfluenceWeights[0] != 0 || OutWeights[1].InfluenceWeights[0] != 0;
    TestTrue(TEXT("copied weights are non-zero (not a zero-fill no-op)"), bAnyNonZeroWeight);

    // Empty source must report a failed transfer (0 written), so the handler can
    // surface NO_SOURCE_WEIGHTS instead of silently producing a zeroed profile.
    TArray<FRawSkinWeight> EmptyOut;
    const int32 CopiedFromEmpty = SkinWeightTransferUtils::CopyClosestVertexWeights(
        TArray<FSoftSkinVertex>(), TargetVertices, EmptyOut);
    TestEqual(TEXT("empty source copies nothing"), CopiedFromEmpty, 0);

    // Second half of the transfer: the copied SkinWeights are rebuilt into the
    // pre-chunk FVertInfluence list that Build() re-chunks from. A profile with
    // populated SkinWeights but empty SourceModelInfluences comes back empty after
    // Build(), so this list must be non-empty and de-quantized from the uint16 weights.
    //
    // The rebuild also crosses an index space, which is why it needs a LOD model. The copied
    // FRawSkinWeight slots are SECTION-LOCAL (indices into FSkelMeshSection::BoneMap) while
    // SourceModelInfluences::BoneIndex is unioned straight into the rebuilt chunk bone map by
    // SkeletalMeshTools::ChunkSkinnedVertices and must therefore be REFERENCE-SKELETON. The
    // fixture below gives the section a deliberately NON-identity bone map so an implementation
    // that skips the indirection cannot pass: slots 3/7/9 map to real bones 30/70/90.
    FSkeletalMeshLODModel RebuildLOD;
    {
        FSkelMeshSection& Section = RebuildLOD.Sections.AddDefaulted_GetRef();
        Section.BoneMap = { 0, 10, 20, 30, 40, 50, 60, 70, 80, 90 };
        Section.SoftVertices.SetNum(2);
        Section.NumVertices = 2;
        Section.BaseVertexIndex = 0;
        RebuildLOD.NumVertices = 2;
    }

    TArray<SkeletalMeshImportData::FVertInfluence> Influences;
    const int32 Dropped =
        SkinWeightTransferUtils::RebuildSourceModelInfluences(RebuildLOD, OutWeights, Influences);
    TestEqual(TEXT("every copied influence mapped through the bone map"), Dropped, 0);

    // T0 has one influence (slot 3, full weight); T1 has two (slots 7 and 9). Trailing
    // zero-padded slots are skipped, so 3 entries total — not 2 * MAX_TOTAL_INFLUENCES.
    TestEqual(TEXT("rebuilt influence count skips zero-padding"), Influences.Num(), 3);
    if (Influences.Num() != 3)
    {
        return false;
    }

    // T0's single influence: vert 0, section-local slot 3 -> reference-skeleton bone 30,
    // de-quantized weight ~= 1.0 (65535/65535). A rebuild that wrote the raw slot yields 3.
    TestEqual(TEXT("influence[0] vert index"), static_cast<int32>(Influences[0].VertIndex), 0);
    TestEqual(TEXT("influence[0] bone index is the MAPPED bone, not the section slot"),
        static_cast<int32>(Influences[0].BoneIndex), 30);
    TestEqual(TEXT("influence[0] de-quantized weight"), Influences[0].Weight, 1.0f, 1.0e-4f);

    // T1's two influences: vert 1, slots 7 then 9 -> bones 70 and 90, weights de-quantized
    // from 40000 / 25535.
    TestEqual(TEXT("influence[1] vert index"), static_cast<int32>(Influences[1].VertIndex), 1);
    TestEqual(TEXT("influence[1] bone index is the MAPPED bone, not the section slot"),
        static_cast<int32>(Influences[1].BoneIndex), 70);
    TestEqual(TEXT("influence[1] de-quantized weight"),
        Influences[1].Weight, SkinWeightTransferUtils::RawWeightToFloat(40000), 1.0e-4f);
    TestEqual(TEXT("influence[2] vert index"), static_cast<int32>(Influences[2].VertIndex), 1);
    TestEqual(TEXT("influence[2] bone index is the MAPPED bone, not the section slot"),
        static_cast<int32>(Influences[2].BoneIndex), 90);
    TestEqual(TEXT("influence[2] de-quantized weight"),
        Influences[2].Weight, SkinWeightTransferUtils::RawWeightToFloat(25535), 1.0e-4f);

    return true;
}

// Regression for F-skeleton-skin-weight-profile-readback: the skin-weight mutators had no
// readback, so a zero-filled or un-normalized profile (the masked
// B-skeleton-copy-weights-noop-zero-fill defect) was invisible through the API. This drives
// the production validity-summary math (SkinWeightTransferUtils::SummarizeSkinWeights, the
// exact function the skeleton.describe_skin_weights handler calls) and asserts it classifies
// a normalized vertex, a zero-filled vertex, and a degenerate (un-normalized) vertex
// distinctly — so the readback can surface a no-op zero-fill. If the summary collapsed those
// buckets (e.g. counting a zero-filled vertex as normalized), these assertions fail.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSkelMeshDescribeSkinWeightsSummaryClassifiesValidityTest,
    "PinWright.skeleton.describe_skin_weights.SummaryClassifiesValidity",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSkelMeshDescribeSkinWeightsSummaryClassifiesValidityTest::RunTest(const FString& Parameters)
{
    using namespace SkinWeightTransferUtils;

    TArray<FRawSkinWeight> SkinWeights;

    // V0: a valid, normalized vertex — two influences summing to 65535 (== 1.0).
    {
        FRawSkinWeight W;
        FMemory::Memzero(&W, sizeof(FRawSkinWeight));
        W.InfluenceBones[0] = 2;
        W.InfluenceWeights[0] = 40000;
        W.InfluenceBones[1] = 5;
        W.InfluenceWeights[1] = 25535;  // 40000 + 25535 == 65535 -> sum 1.0
        SkinWeights.Add(W);
    }
    // V1: the zero-fill signature — no influences at all (the masked copy_weights bug).
    {
        FRawSkinWeight W;
        FMemory::Memzero(&W, sizeof(FRawSkinWeight));
        SkinWeights.Add(W);
    }
    // V2: a single full-weight influence — one influence, sum 1.0 (normalized).
    {
        FRawSkinWeight W;
        FMemory::Memzero(&W, sizeof(FRawSkinWeight));
        W.InfluenceBones[0] = 7;
        W.InfluenceWeights[0] = 65535;
        SkinWeights.Add(W);
    }
    // V3: degenerate — has influence but sums to ~0.5, not 1.0 (un-normalized).
    {
        FRawSkinWeight W;
        FMemory::Memzero(&W, sizeof(FRawSkinWeight));
        W.InfluenceBones[0] = 1;
        W.InfluenceWeights[0] = 32767;  // ~0.5
        SkinWeights.Add(W);
    }

    const FSkinWeightProfileSummary Summary = SummarizeSkinWeights(SkinWeights, /*SampleCount*/ 2);

    TestEqual(TEXT("vertex count"), Summary.VertexCount, 4);
    // V0 has two non-zero influences; that is the per-vertex max.
    TestEqual(TEXT("max influences per vertex"), Summary.MaxInfluencesPerVertex, 2);
    // V0 and V2 sum to 1.0.
    TestEqual(TEXT("normalized vertex count"), Summary.NormalizedVertexCount, 2);
    // V1 is the zero-fill — counted distinctly so a no-op is observable.
    TestEqual(TEXT("zero-weight vertex count surfaces the zero-fill"), Summary.ZeroWeightVertexCount, 1);
    // V3 has influence but does not sum to 1.0.
    TestEqual(TEXT("degenerate vertex count"), Summary.DegenerateVertexCount, 1);

    // Sample budget honored: first 2 vertices only.
    TestEqual(TEXT("sample honors sampleCount budget"), Summary.Samples.Num(), 2);
    if (Summary.Samples.Num() == 2)
    {
        // V0 sample: two influences, de-quantized weights summing to ~1.0.
        TestEqual(TEXT("sample[0] vertex index"), Summary.Samples[0].VertexIndex, 0);
        TestEqual(TEXT("sample[0] influence count drops zero padding"), Summary.Samples[0].BoneIndices.Num(), 2);
        TestEqual(TEXT("sample[0] first bone"), Summary.Samples[0].BoneIndices[0], 2);
        TestEqual(TEXT("sample[0] weight sum ~= 1.0"), Summary.Samples[0].WeightSum, 1.0f, 1.0e-3f);
        // V1 sample: the zero-fill vertex has no influences and a zero weight sum.
        TestEqual(TEXT("sample[1] vertex index"), Summary.Samples[1].VertexIndex, 1);
        TestEqual(TEXT("sample[1] zero-fill has no influences"), Summary.Samples[1].BoneIndices.Num(), 0);
        TestEqual(TEXT("sample[1] zero-fill weight sum is 0"), Summary.Samples[1].WeightSum, 0.0f, 1.0e-6f);
    }

    // An empty profile (no vertices) summarizes cleanly to all-zero counts.
    const FSkinWeightProfileSummary Empty = SummarizeSkinWeights(TArray<FRawSkinWeight>(), 0);
    TestEqual(TEXT("empty profile vertex count"), Empty.VertexCount, 0);
    TestEqual(TEXT("empty profile collects no samples"), Empty.Samples.Num(), 0);

    return true;
}

// Regression for B-skeleton-auto-skin-weights-noop-rebuild: skeleton.normalize_weights and
// skeleton.prune_weights used to be pure no-ops — they called Mesh->Build() and reported
// success without touching a single influence (and prune_weights accept-then-discarded its
// threshold). This drives the production weight-edit math (SkinWeightTransferUtils::
// NormalizeSkinWeights / PruneSkinWeights, the exact functions the handlers now call) and
// asserts each actually rewrites the per-vertex influences. Under the old no-op handlers
// there was no such helper and the weights were never altered, so these assertions guard
// against a revert to a bare-Build() body.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSkelMeshNormalizePruneWeightsMathTest,
    "PinWright.skeleton.normalize_prune_weights.RealMath",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSkelMeshNormalizePruneWeightsMathTest::RunTest(const FString& Parameters)
{
    using namespace SkinWeightTransferUtils;

    // --- normalize: an un-normalized vertex (sum ~0.5) gets rescaled to sum 1.0 ---
    {
        TArray<FRawSkinWeight> SkinWeights;

        // V0: two influences summing to 32767 (~0.5) — un-normalized, must be rescaled.
        FRawSkinWeight Degenerate;
        FMemory::Memzero(&Degenerate, sizeof(FRawSkinWeight));
        Degenerate.InfluenceBones[0] = 2;
        Degenerate.InfluenceWeights[0] = 20000;
        Degenerate.InfluenceBones[1] = 5;
        Degenerate.InfluenceWeights[1] = 12767;  // 20000 + 12767 = 32767 (~0.5)
        SkinWeights.Add(Degenerate);

        // V1: already normalized (single full-weight influence) — must be left unchanged.
        FRawSkinWeight Normalized;
        FMemory::Memzero(&Normalized, sizeof(FRawSkinWeight));
        Normalized.InfluenceBones[0] = 7;
        Normalized.InfluenceWeights[0] = 65535;
        SkinWeights.Add(Normalized);

        const FWeightArrayEditResult NormResult = NormalizeSkinWeights(SkinWeights);

        // Only the degenerate vertex changed.
        TestEqual(TEXT("normalize changed exactly the un-normalized vertex"), NormResult.VerticesChanged, 1);

        // V0 now sums to ~1.0 (the real renormalization the old no-op never performed).
        float Sum0 = 0.0f;
        ForEachNonZeroInfluence(SkinWeights[0], [&](int32, float W) { Sum0 += W; });
        TestEqual(TEXT("normalized vertex now sums to 1.0"), Sum0, 1.0f, NormalizedWeightSumTolerance);

        // The bone assignment (which influence belongs to which bone) is preserved.
        TestEqual(TEXT("normalize keeps first bone"), static_cast<int32>(SkinWeights[0].InfluenceBones[0]), 2);
        TestEqual(TEXT("normalize keeps second bone"), static_cast<int32>(SkinWeights[0].InfluenceBones[1]), 5);

        // V1 was already normalized — untouched.
        TestEqual(TEXT("already-normalized vertex untouched"),
            static_cast<int32>(SkinWeights[1].InfluenceWeights[0]), 65535);
    }

    // --- prune: a tiny influence below threshold is dropped, survivors renormalized ---
    {
        TArray<FRawSkinWeight> SkinWeights;

        // V0: one dominant influence + one tiny one (~0.0076, below a 0.05 threshold).
        FRawSkinWeight W;
        FMemory::Memzero(&W, sizeof(FRawSkinWeight));
        W.InfluenceBones[0] = 4;
        W.InfluenceWeights[0] = 65035;  // ~0.9924
        W.InfluenceBones[1] = 11;
        W.InfluenceWeights[1] = 500;    // ~0.0076 -> below threshold, pruned
        SkinWeights.Add(W);

        const FWeightArrayEditResult PruneResult = PruneSkinWeights(SkinWeights, /*Threshold*/ 0.05f);

        // The tiny influence was removed (threshold is now actually applied, not discarded).
        TestEqual(TEXT("prune removed exactly one influence"), PruneResult.InfluencesRemoved, 1);
        TestEqual(TEXT("prune changed exactly one vertex"), PruneResult.VerticesChanged, 1);

        // Only the dominant influence survives, renormalized to ~1.0.
        int32 SurvivingInfluences = 0;
        float Sum = 0.0f;
        ForEachNonZeroInfluence(SkinWeights[0], [&](int32, float Wt) { ++SurvivingInfluences; Sum += Wt; });
        TestEqual(TEXT("one influence survives the prune"), SurvivingInfluences, 1);
        TestEqual(TEXT("surviving bone is the dominant one"),
            static_cast<int32>(SkinWeights[0].InfluenceBones[0]), 4);
        TestEqual(TEXT("pruned vertex renormalized to 1.0"), Sum, 1.0f, NormalizedWeightSumTolerance);
    }

    // --- prune safety: a threshold that would strip every influence is a no-op (never zero-fill) ---
    {
        TArray<FRawSkinWeight> SkinWeights;
        FRawSkinWeight W;
        FMemory::Memzero(&W, sizeof(FRawSkinWeight));
        W.InfluenceBones[0] = 3;
        W.InfluenceWeights[0] = 30000;  // sole influence, ~0.458 — below the 0.9 threshold below
        SkinWeights.Add(W);

        // A 0.9 threshold puts the sole ~0.458 influence below the cut, so pruning it
        // would leave the vertex with zero influences. The helper must refuse (drop none,
        // leave the vertex byte-for-byte intact) rather than write that unskinned zero-fill
        // — the destructive direction the ticket warns about.
        const FWeightArrayEditResult PruneAll = PruneSkinWeights(SkinWeights, /*Threshold*/ 0.9f);
        TestEqual(TEXT("dropping all influences removes none (no zero-fill)"), PruneAll.InfluencesRemoved, 0);
        TestEqual(TEXT("vertex left intact"), static_cast<int32>(SkinWeights[0].InfluenceWeights[0]), 30000);
        TestEqual(TEXT("vertex bone left intact"), static_cast<int32>(SkinWeights[0].InfluenceBones[0]), 3);
    }

    return true;
}

// Builds one in-code FRawSkinWeight influence entry: zero-fills every slot (so trailing
// influences read as "none") then assigns slot i from each {bone, quantizedWeight} pair.
// Centralizes the memzero-then-assign idiom the weight-edit tests need, so a new caller
// can't silently leave garbage in the unassigned influence slots by forgetting the Memzero.
static FRawSkinWeight MakeRawWeight(std::initializer_list<TPair<uint16, uint16>> Influences)
{
    FRawSkinWeight Weight;
    FMemory::Memzero(&Weight, sizeof(FRawSkinWeight));
    int32 Slot = 0;
    for (const TPair<uint16, uint16>& Influence : Influences)
    {
        Weight.InfluenceBones[Slot] = Influence.Key;
        Weight.InfluenceWeights[Slot] = Influence.Value;
        ++Slot;
    }
    return Weight;
}

// FSoftSkinVertex-side counterpart of MakeRawWeight for the section base-skinning the
// weight-edit tests seed from: zero-fills both influence arrays (so trailing slots read as
// "none") then assigns slot 0 to Bone @ full weight. Centralizes the memzero-then-assign
// idiom so a new caller can't silently leave garbage in the unassigned influence slots by
// forgetting the Memzero — the same footgun MakeRawWeight closes, but for the section side.
static void SetSectionBaseBone(FSkelMeshSection& Section, int32 VertexIndex, uint16 Bone)
{
    FSoftSkinVertex& Vertex = Section.SoftVertices[VertexIndex];
    FMemory::Memzero(Vertex.InfluenceBones, sizeof(Vertex.InfluenceBones));
    FMemory::Memzero(Vertex.InfluenceWeights, sizeof(Vertex.InfluenceWeights));
    Vertex.InfluenceBones[0] = Bone;
    Vertex.InfluenceWeights[0] = 65535;  // full weight -> sums to 1.0
}

// Regression for B-skeleton-normalize-recaptures-base-clobbers-profile: when
// normalize_weights / prune_weights target a profile that set_vertex_weights already
// authored, the shared ApplyWeightEditToProfile scaffold used to ALWAYS seed its working
// array from the LOD's base section skinning (CaptureBaseSkinWeights) and never read the
// target profile — so WriteSkinWeightProfile then overwrote the authored profile with a
// fresh base-skinning capture (silent data loss), while reporting verticesNormalized:0
// because base skinning already sums to ~1.0. The fix routes the seed through the pure
// SkinWeightTransferUtils::SeedWeightEditSource, which reads the profile's existing
// SkinWeights when populated and only falls back to base skinning when the profile is
// absent/empty. This drives that production helper directly against an in-code LOD model
// (no Build() DDC pipeline, no example content needed): a section carrying distinct base
// skinning plus a named profile carrying different authored weights. Pre-fix behavior
// (always base) makes the "seeds from authored profile" assertions fail, guarding the revert.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSkelMeshWeightEditSeedsFromProfileTest,
    "PinWright.skeleton.normalize_weights.SeedsFromAuthoredProfile",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSkelMeshWeightEditSeedsFromProfileTest::RunTest(const FString& Parameters)
{
    using namespace SkinWeightTransferUtils;

    // Build a 2-vertex LOD model whose single section carries a DISTINCT base skinning:
    // v0 -> bone 2 @ full, v1 -> bone 5 @ full. GetVertices() reads Sections[*].SoftVertices,
    // so NumVertices must equal the section vertex count or the engine logs Fatal.
    FSkeletalMeshLODModel LODModel;
    FSkelMeshSection& Section = LODModel.Sections.AddDefaulted_GetRef();
    Section.SoftVertices.SetNum(2);
    SetSectionBaseBone(Section, 0, 2);
    SetSectionBaseBone(Section, 1, 5);
    LODModel.NumVertices = 2;

    // Author a DIFFERENT weighting into a named profile P (as set_vertex_weights would):
    // v0 -> bones {7:0.5, 9:0.5}, v1 -> bone 3 @ full. These must never be mistaken for base.
    const FName ProfileName(TEXT("CustomWeights"));
    FImportedSkinWeightProfileData& ProfileData = LODModel.SkinWeightProfiles.FindOrAdd(ProfileName);
    ProfileData.SkinWeights = {
        MakeRawWeight({{7, 32767}, {9, 32768}}),  // v0 -> ~0.5/~0.5
        MakeRawWeight({{3, 65535}}),              // v1 -> bone 3 @ full
    };

    // --- populated profile: the edit must seed from the AUTHORED profile, not base ---
    {
        TArray<FRawSkinWeight> Seed;
        const bool bFromProfile = SeedWeightEditSource(LODModel, ProfileName, Seed);

        // Pre-fix: always seeded from base -> bFromProfile would be false and the bones
        // below would be the base 2/5, destroying the authored 7/9/3 on write.
        TestTrue(TEXT("populated profile is the edit source (not base skinning)"), bFromProfile);
        TestEqual(TEXT("seed vertex count matches LOD"), Seed.Num(), 2);
        if (Seed.Num() == 2)
        {
            TestEqual(TEXT("v0 first bone is authored (7), not base (2)"),
                static_cast<int32>(Seed[0].InfluenceBones[0]), 7);
            TestEqual(TEXT("v0 second authored influence preserved (9)"),
                static_cast<int32>(Seed[0].InfluenceBones[1]), 9);
            TestEqual(TEXT("v1 bone is authored (3), not base (5)"),
                static_cast<int32>(Seed[1].InfluenceBones[0]), 3);
        }

        // End-to-end intent: normalizing that seed leaves the authored bone ASSIGNMENTS
        // intact (v0's ~0.5/~0.5 renormalizes to sum 1.0 without changing which bones);
        // the whole point is that the authored profile is edited in place, not clobbered.
        // We only want the in-place mutation of Seed; NormalizeSkinWeights reports v0
        // unchanged (its authored ~0.5+~0.5 already sums to ~1.0), so the result is unused —
        // what matters for the regression is the SOURCE, asserted above.
        NormalizeSkinWeights(Seed);
        TestEqual(TEXT("normalized authored v0 still on bone 7"),
            static_cast<int32>(Seed[0].InfluenceBones[0]), 7);
        TestEqual(TEXT("normalized authored v1 still on bone 3"),
            static_cast<int32>(Seed[1].InfluenceBones[0]), 3);
        float Sum0 = 0.0f;
        ForEachNonZeroInfluence(Seed[0], [&](int32, float W) { Sum0 += W; });
        TestEqual(TEXT("authored v0 renormalized to 1.0 in place"), Sum0, 1.0f, NormalizedWeightSumTolerance);
    }

    // --- absent/empty profile: the edit must fall back to base skinning ---
    {
        TArray<FRawSkinWeight> Seed;
        const bool bFromProfile = SeedWeightEditSource(LODModel, FName(TEXT("FreshProfile")), Seed);

        // A profile that does not exist on this LOD has nothing to edit in place, so the
        // seed must come from the LOD's base section skinning (the pre-existing behavior for
        // a fresh target profile, which the fix must preserve).
        TestFalse(TEXT("absent profile falls back to base skinning"), bFromProfile);
        TestEqual(TEXT("base seed vertex count matches LOD"), Seed.Num(), 2);
        if (Seed.Num() == 2)
        {
            TestEqual(TEXT("base v0 bone is the section's (2)"),
                static_cast<int32>(Seed[0].InfluenceBones[0]), 2);
            TestEqual(TEXT("base v1 bone is the section's (5)"),
                static_cast<int32>(Seed[1].InfluenceBones[0]), 5);
        }
    }

    return true;
}

// Regression for B-skeleton-describe-skin-weights-garbage-on-fresh-profile: set_vertex_weights
// used to grow a fresh profile's DENSE per-vertex buffer with a plain SetNum, which
// default-constructs the trivial POD FRawSkinWeight and so left every vertex the call did NOT
// author holding uninitialized garbage (out-of-range bone indices like 33537 on a small
// skeleton). It then zeroed+authored only the named vertices. describe_skin_weights faithfully
// serialized that buffer, reporting a 4-vertex edit as a ~95%-degenerate profile with impossible
// bone indices. The fix seeds the whole buffer from the LOD's base section skinning
// (SkinWeightTransferUtils::SeedWeightEditSource) before applying the authored overrides on top,
// then persists via WriteSkinWeightProfile. This drives that exact seed+author+summarize
// sequence over an in-code LOD model (no Build() DDC, no example content): a section carrying
// known base skinning, ONE authored override vertex, and the production validity summary.
// Under the reverted SetNum-only writer the un-authored vertices would carry garbage, so the
// "every un-authored vertex holds its valid base influence" and "zero degenerate" assertions
// would fail (they'd be classified degenerate with out-of-range bones), guarding the revert.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSkelMeshSetVertexWeightsSeedsUnauthoredFromBaseTest,
    "PinWright.skeleton.set_vertex_weights.SeedsUnauthoredFromBase",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSkelMeshSetVertexWeightsSeedsUnauthoredFromBaseTest::RunTest(const FString& Parameters)
{
    using namespace SkinWeightTransferUtils;

    // A 4-vertex LOD whose single section carries a DISTINCT, valid base skinning:
    // v0->bone 2, v1->bone 5, v2->bone 8, v3->bone 11 (each full weight). GetVertices() reads
    // Sections[*].SoftVertices, so NumVertices must equal the section vertex count.
    FSkeletalMeshLODModel LODModel;
    FSkelMeshSection& Section = LODModel.Sections.AddDefaulted_GetRef();
    Section.SoftVertices.SetNum(4);
    const uint16 BaseBones[4] = {2, 5, 8, 11};
    for (int32 V = 0; V < 4; ++V)
    {
        SetSectionBaseBone(Section, V, BaseBones[V]);
    }
    LODModel.NumVertices = 4;

    // Mirror the fixed set_vertex_weights body for a FRESH profile (profileCount:0 before):
    // (1) seed the whole dense buffer, (2) author ONE override vertex on top.
    const FName ProfileName(TEXT("CustomWeights"));
    TArray<FRawSkinWeight> SkinWeights;
    const bool bFromProfile = SeedWeightEditSource(LODModel, ProfileName, SkinWeights);

    // A fresh profile has no authored data, so the seed MUST fall back to base skinning and
    // size the buffer to EVERY vertex — not leave the un-named ones uninitialized.
    TestFalse(TEXT("fresh profile seeds from base skinning, not an existing profile"), bFromProfile);
    TestEqual(TEXT("seed covers every LOD vertex (dense buffer)"), SkinWeights.Num(), 4);
    if (SkinWeights.Num() != 4)
    {
        return false;
    }

    // Author a single override into v1 (as a 1-vertex set_vertex_weights would): bone 20 @ full.
    // The other three vertices are the ones a plain SetNum would have left as garbage.
    const int32 AuthoredVertex = 1;
    const uint16 AuthoredBone = 20;
    FMemory::Memzero(&SkinWeights[AuthoredVertex], sizeof(FRawSkinWeight));
    SkinWeights[AuthoredVertex].InfluenceBones[0] = AuthoredBone;
    SkinWeights[AuthoredVertex].InfluenceWeights[0] = FloatToRawWeight(1.0f);

    // The authored vertex carries exactly the override.
    TestEqual(TEXT("authored vertex holds the override bone"),
        static_cast<int32>(SkinWeights[AuthoredVertex].InfluenceBones[0]), static_cast<int32>(AuthoredBone));

    // Every UN-AUTHORED vertex must still hold its valid base influence (a real, in-range bone),
    // NOT uninitialized garbage. This is the core of the fix: a plain SetNum would leave these
    // slots indeterminate; seeding from base fills them with the section's own skinning.
    for (int32 V = 0; V < 4; ++V)
    {
        if (V == AuthoredVertex)
        {
            continue;
        }
        TestEqual(TEXT("un-authored vertex retains its valid base bone (not garbage)"),
            static_cast<int32>(SkinWeights[V].InfluenceBones[0]), static_cast<int32>(BaseBones[V]));
    }

    // Readback parity: the production validity summary (the exact math describe_skin_weights
    // runs) must see a fully-VALID profile — every vertex normalized, ZERO degenerate, ZERO
    // zero-weight. Pre-fix the un-authored vertices were garbage, so they classified as
    // degenerate (or surfaced out-of-range sampled bones), inflating DegenerateVertexCount.
    const FSkinWeightProfileSummary Summary = SummarizeSkinWeights(SkinWeights, /*SampleCount*/ 4);
    TestEqual(TEXT("summary sees every vertex"), Summary.VertexCount, 4);
    TestEqual(TEXT("all vertices normalized (authored + base-seeded)"), Summary.NormalizedVertexCount, 4);
    TestEqual(TEXT("no degenerate vertices from an uninitialized buffer"), Summary.DegenerateVertexCount, 0);
    TestEqual(TEXT("no zero-weight vertices"), Summary.ZeroWeightVertexCount, 0);

    // No sampled influence may reference a bone outside the skeleton's range — an out-of-range
    // index is the uninitialized-memory signature the ticket flagged (e.g. 33537). All authored
    // and base bones here are <= 20, so a garbage seed is the only way this fails.
    const int32 MaxPlausibleBone = 32;
    for (const FSkinWeightVertexSample& Sample : Summary.Samples)
    {
        for (int32 BoneIdx : Sample.BoneIndices)
        {
            TestTrue(TEXT("sampled bone index is in range (no uninitialized garbage)"),
                BoneIdx >= 0 && BoneIdx <= MaxPlausibleBone);
        }
    }

    return true;
}

// ============================================================================
// SkeletonHandler.cpp — 16 handlers
// ============================================================================

// skeleton.get_info — all params optional; empty payload exercises the not-found path.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSkeletonGetInfoValidNoCrashTest,
    "PinWright.skeleton.get_info.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSkeletonGetInfoValidNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    TestTrue(TEXT("skeleton.get_info handler found"),
        InvokeHandler(TEXT("skeleton.get_info"), Payload));
    return true;
}

// skeleton.list_bones — all params optional; empty payload exercises the not-found path.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSkeletonListBonesValidNoCrashTest,
    "PinWright.skeleton.list_bones.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSkeletonListBonesValidNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    TestTrue(TEXT("skeleton.list_bones handler found"),
        InvokeHandler(TEXT("skeleton.list_bones"), Payload));
    return true;
}

// ============================================================================
// skeleton.list_bones — nameFilter / limit / namesOnly narrowing
// (board E-skeleton-list-bones-no-limit-spills)
//
// Builds a real on-disk USkeleton with a fixed 5-bone hierarchy (two bones share
// the "hand" substring) so the handler's path-based StaticLoadObject resolves it,
// then drives the production handler and asserts: (1) the default un-narrowed call
// returns all five bones with the full per-bone shape and totalCount==count /
// truncated==false; (2) limit=2 caps the returned rows at 2 while totalCount stays
// 5 and truncated flips true; (3) namesOnly drops the location/index/parent fields,
// leaving only name; (4) a nameFilter substring returns only the matching bones.
// Reverting the limit/projection/filter plumbing fails this test.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSkeletonListBonesNarrowingTest,
    "PinWright.skeleton.list_bones.Narrowing",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSkeletonListBonesNarrowingTest::RunTest(const FString& Parameters)
{
    // A real package on disk: the handler resolves the skeleton by path via
    // StaticLoadObject, which a bare transient object would not satisfy. GUID-suffixed
    // so parallel/repeat runs never collide.
    const FString PkgPath = FString::Printf(TEXT("/Game/PinWrightTest_SK_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    const FString AssetName = FPackageName::GetLongPackageAssetName(PkgPath);

    UPackage* Package = CreatePackage(*PkgPath);
    TestNotNull(TEXT("test package created"), Package);
    if (!Package) return false;
    ON_SCOPE_EXIT { CleanupTestAsset(PkgPath); };

    USkeleton* Skeleton = NewObject<USkeleton>(
        Package, *AssetName, RF_Public | RF_Standalone | RF_Transactional);
    TestNotNull(TEXT("skeleton created"), Skeleton);
    if (!Skeleton) return false;

    // Fixed hierarchy: root -> spine_01 -> {hand_r, hand_l, foot_r}. Two bones share
    // the "hand" substring so the nameFilter case matches exactly two of the five.
    {
        FReferenceSkeletonModifier Modifier(Skeleton);
        Modifier.Add(FMeshBoneInfo(FName(TEXT("root")), TEXT("root"), INDEX_NONE),
            FTransform::Identity, true);
        Modifier.Add(FMeshBoneInfo(FName(TEXT("spine_01")), TEXT("spine_01"), 0),
            FTransform(FVector(0.0, 0.0, 10.0)));
        Modifier.Add(FMeshBoneInfo(FName(TEXT("hand_r")), TEXT("hand_r"), 1),
            FTransform(FVector(20.0, 0.0, 15.0)));
        Modifier.Add(FMeshBoneInfo(FName(TEXT("hand_l")), TEXT("hand_l"), 1),
            FTransform(FVector(-20.0, 0.0, 15.0)));
        Modifier.Add(FMeshBoneInfo(FName(TEXT("foot_r")), TEXT("foot_r"), 1),
            FTransform(FVector(10.0, 0.0, -30.0)));
    }
    TestEqual(TEXT("fixture skeleton has 5 raw bones"),
        Skeleton->GetReferenceSkeleton().GetRawBoneNum(), 5);
    const FString SkeletonPath = Skeleton->GetPathName();

    // (1) Default: all five bones, full per-bone shape, no truncation.
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("skeletonPath"), SkeletonPath);

        FTestResponseCapture Capture;
        TestTrue(TEXT("list_bones handler found (default)"),
            InvokeHandlerWithCapture(TEXT("skeleton.list_bones"), Payload, Capture));
        TestTrue(TEXT("default call succeeds"), Capture.bSuccess);
        if (Capture.bSuccess && Capture.Result.IsValid())
        {
            const TArray<TSharedPtr<FJsonValue>>* Bones = nullptr;
            TestTrue(TEXT("bones array present"),
                Capture.Result->TryGetArrayField(TEXT("bones"), Bones));
            if (Bones)
            {
                TestEqual(TEXT("default returns all 5 bones"), Bones->Num(), 5);
            }
            double Count = 0, TotalCount = 0;
            Capture.Result->TryGetNumberField(TEXT("count"), Count);
            Capture.Result->TryGetNumberField(TEXT("totalCount"), TotalCount);
            TestEqual(TEXT("count==5"), (int32)Count, 5);
            TestEqual(TEXT("totalCount==5"), (int32)TotalCount, 5);
            bool bTruncated = true;
            Capture.Result->TryGetBoolField(TEXT("truncated"), bTruncated);
            TestFalse(TEXT("default not truncated"), bTruncated);

            // Full per-bone shape is preserved by default (index + location present).
            if (Bones && Bones->Num() > 0)
            {
                const TSharedPtr<FJsonObject>* First = nullptr;
                if ((*Bones)[0]->TryGetObject(First) && First)
                {
                    TestTrue(TEXT("default row carries location"),
                        (*First)->HasField(TEXT("location")));
                    double Index = -1;
                    TestTrue(TEXT("default row carries index"),
                        (*First)->TryGetNumberField(TEXT("index"), Index));
                }
            }
        }
    }

    // (2) limit=2 caps returned rows but totalCount/truncated expose the elision.
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("skeletonPath"), SkeletonPath);
        Payload->SetNumberField(TEXT("limit"), 2);

        FTestResponseCapture Capture;
        TestTrue(TEXT("list_bones handler found (limit)"),
            InvokeHandlerWithCapture(TEXT("skeleton.list_bones"), Payload, Capture));
        if (Capture.bSuccess && Capture.Result.IsValid())
        {
            const TArray<TSharedPtr<FJsonValue>>* Bones = nullptr;
            Capture.Result->TryGetArrayField(TEXT("bones"), Bones);
            if (Bones)
            {
                TestEqual(TEXT("limit=2 returns 2 rows"), Bones->Num(), 2);
            }
            double Count = 0, TotalCount = 0;
            Capture.Result->TryGetNumberField(TEXT("count"), Count);
            Capture.Result->TryGetNumberField(TEXT("totalCount"), TotalCount);
            TestEqual(TEXT("limited count==2"), (int32)Count, 2);
            TestEqual(TEXT("limited totalCount stays 5"), (int32)TotalCount, 5);
            bool bTruncated = false;
            Capture.Result->TryGetBoolField(TEXT("truncated"), bTruncated);
            TestTrue(TEXT("limited result is truncated"), bTruncated);
        }
    }

    // (3) namesOnly drops index/parent/location, leaving name.
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("skeletonPath"), SkeletonPath);
        Payload->SetBoolField(TEXT("namesOnly"), true);

        FTestResponseCapture Capture;
        TestTrue(TEXT("list_bones handler found (namesOnly)"),
            InvokeHandlerWithCapture(TEXT("skeleton.list_bones"), Payload, Capture));
        if (Capture.bSuccess && Capture.Result.IsValid())
        {
            const TArray<TSharedPtr<FJsonValue>>* Bones = nullptr;
            Capture.Result->TryGetArrayField(TEXT("bones"), Bones);
            if (Bones && Bones->Num() > 0)
            {
                const TSharedPtr<FJsonObject>* First = nullptr;
                if ((*Bones)[0]->TryGetObject(First) && First)
                {
                    FString Name;
                    TestTrue(TEXT("namesOnly row keeps name"),
                        (*First)->TryGetStringField(TEXT("name"), Name));
                    TestFalse(TEXT("namesOnly row drops location"),
                        (*First)->HasField(TEXT("location")));
                    double Index = 0;
                    TestFalse(TEXT("namesOnly row drops index"),
                        (*First)->TryGetNumberField(TEXT("index"), Index));
                }
            }
        }
    }

    // (4) nameFilter substring returns only the matching bones (hand_r + hand_l).
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("skeletonPath"), SkeletonPath);
        Payload->SetStringField(TEXT("nameFilter"), TEXT("hand"));

        FTestResponseCapture Capture;
        TestTrue(TEXT("list_bones handler found (filter)"),
            InvokeHandlerWithCapture(TEXT("skeleton.list_bones"), Payload, Capture));
        if (Capture.bSuccess && Capture.Result.IsValid())
        {
            const TArray<TSharedPtr<FJsonValue>>* Bones = nullptr;
            Capture.Result->TryGetArrayField(TEXT("bones"), Bones);
            if (Bones)
            {
                TestEqual(TEXT("filter 'hand' returns 2 bones"), Bones->Num(), 2);
                for (const TSharedPtr<FJsonValue>& Val : *Bones)
                {
                    const TSharedPtr<FJsonObject>* Obj = nullptr;
                    if (Val->TryGetObject(Obj) && Obj)
                    {
                        FString Name;
                        (*Obj)->TryGetStringField(TEXT("name"), Name);
                        TestTrue(TEXT("filtered bone name contains 'hand'"),
                            Name.Contains(TEXT("hand")));
                    }
                }
            }
            double TotalCount = 0;
            Capture.Result->TryGetNumberField(TEXT("totalCount"), TotalCount);
            TestEqual(TEXT("filtered totalCount==2"), (int32)TotalCount, 2);
        }
    }

    return true;
}

// skeleton.list_virtual_bones — all params optional; empty payload exercises the missing-param path.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSkeletonListVirtualBonesValidNoCrashTest,
    "PinWright.skeleton.list_virtual_bones.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSkeletonListVirtualBonesValidNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    TestTrue(TEXT("skeleton.list_virtual_bones handler found"),
        InvokeHandler(TEXT("skeleton.list_virtual_bones"), Payload));
    return true;
}

// skeleton.list_sockets — all params optional; empty payload exercises the not-found path.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSkeletonListSocketsValidNoCrashTest,
    "PinWright.skeleton.list_sockets.ValidParamsNoCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSkeletonListSocketsValidNoCrashTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    TestTrue(TEXT("skeleton.list_sockets handler found"),
        InvokeHandler(TEXT("skeleton.list_sockets"), Payload));
    return true;
}

// ============================================================================
// IK Rig / IK Retargeter authoring family — ticket F-ik-rig-retargeter-family-not-compiled
// ============================================================================
//
// Regression guard: the whole family (create_ik_rig / add_ik_chain /
// create_ik_retargeter / set_retarget_chain_mapping) used to compile out because
// PinWright.Build.cs carried no IKRig dependency, so every verb
// hard-errored [NOT_SUPPORTED] at runtime despite the wiki advertising the workflow.
// The fix adds IKRig/IKRigEditor to Build.cs, corrects the MCP_HAS_IKRIG guard to the
// cross-module "Rig/IKRigDefinition.h" include path, and replaces the add_ik_chain /
// set_retarget_chain_mapping echo stubs with real controller calls.
//
// Part A always runs and is the direct revert signal: each family handler must NOT return
// the NOT_SUPPORTED fall-through. Reverting the Build.cs deps or the guard fix re-darkens
// the handlers and these assertions fail.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAuthoringIkRigFamilyEnabledTest,
    "PinWright.animation.authoring.ik_rig_family.Enabled",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FAuthoringIkRigFamilyEnabledTest::RunTest(const FString& Parameters)
{
    // Each family verb, invoked with empty/partial params. We never expect NOT_SUPPORTED:
    // when the module is compiled in the handlers reach their own param validation / real
    // logic and emit a different (or no) error. When the module is compiled out they return
    // exactly NOT_SUPPORTED. Asserting "not NOT_SUPPORTED" is the compiled-in proof.
    const TCHAR* FamilyMethods[] = {
        TEXT("animation.authoring.create_ik_rig"),
        TEXT("animation.authoring.add_ik_chain"),
        TEXT("animation.authoring.create_ik_retargeter"),
        TEXT("animation.authoring.set_retarget_chain_mapping"),
    };

    for (const TCHAR* Method : FamilyMethods)
    {
        FTestResponseCapture Capture;
        const bool bFound = InvokeHandlerWithCapture(Method, MakeShared<FJsonObject>(), Capture);
        TestTrue(*FString::Printf(TEXT("%s handler registered"), Method), bFound);
        TestTrue(*FString::Printf(TEXT("%s responded"), Method), Capture.bWasCalled);
        TestNotEqual(
            *FString::Printf(TEXT("%s is not compiled out (NOT_SUPPORTED)"), Method),
            Capture.ErrorCode, FString(TEXT("NOT_SUPPORTED")));
    }

    return true;
}

#if MCP_TEST_HAS_IKRIG
// Part B (compiled-in only): full round-trip proving the create-side factory body and the
// add_ik_chain real-mutation path went live. create_ik_rig must yield a real
// UIKRigDefinition; add_ik_chain must actually push a retarget chain onto it (visible via
// UIKRigDefinition::GetRetargetChains) — the old echo stub never mutated the asset, so this
// fails if add_ik_chain is reverted to the no-op.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAuthoringIkRigCreateAndAddChainRoundTripTest,
    "PinWright.animation.authoring.create_ik_rig.CreateAndAddChainRoundTrip",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FAuthoringIkRigCreateAndAddChainRoundTripTest::RunTest(const FString& Parameters)
{
    // Build a transient skeletal mesh with a root -> bone_a -> bone_b chain. The IK Rig pulls
    // its bone hierarchy from this mesh, and AddRetargetChain validates start/end bones
    // against it.
    USkeletalMesh* Mesh = NewObject<USkeletalMesh>(GetTransientPackage());
    USkeleton* Skeleton = NewObject<USkeleton>(GetTransientPackage());
    TestNotNull(TEXT("transient skeletal mesh created"), Mesh);
    TestNotNull(TEXT("transient skeleton created"), Skeleton);
    if (!Mesh || !Skeleton)
    {
        return false;
    }
    Mesh->AddToRoot();
    Skeleton->AddToRoot();
    Mesh->SetSkeleton(Skeleton);

    // Single source of truth for the test hierarchy: the skeleton ref-skeleton and the mesh
    // ref-skeleton are distinct objects but must carry the identical bones, since
    // AddRetargetChain validates start/end bones against the mesh while the chain is read off
    // the skeleton. Building both from one lambda keeps them from drifting.
    auto BuildHierarchy = [](FReferenceSkeletonModifier& M)
    {
        M.Add(FMeshBoneInfo(FName(TEXT("root")), TEXT("root"), INDEX_NONE), FTransform::Identity, true);
        M.Add(FMeshBoneInfo(FName(TEXT("bone_a")), TEXT("bone_a"), 0), FTransform::Identity);
        M.Add(FMeshBoneInfo(FName(TEXT("bone_b")), TEXT("bone_b"), 1), FTransform::Identity);
    };
    {
        FReferenceSkeletonModifier Modifier(Skeleton);
        BuildHierarchy(Modifier);
    }
    {
        FReferenceSkeletonModifier MeshModifier(Mesh->GetRefSkeleton(), Skeleton);
        BuildHierarchy(MeshModifier);
    }

    const FString RigName = FString::Printf(
        TEXT("IKR_Test_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    const FString RigFolder = TEXT("/Game/PinWrightTests");
    const FString RigPackagePath = FString::Printf(TEXT("%s/%s"), *RigFolder, *RigName);
    const FString RigObjectPath = FString::Printf(TEXT("%s.%s"), *RigPackagePath, *RigName);

    // create_ik_rig via the production handler — must yield a real UIKRigDefinition asset.
    TSharedPtr<FJsonObject> CreatePayload = MakeShared<FJsonObject>();
    CreatePayload->SetStringField(TEXT("name"), RigName);
    CreatePayload->SetStringField(TEXT("path"), RigFolder);
    CreatePayload->SetBoolField(TEXT("save"), false);

    FTestResponseCapture CreateCapture;
    TestTrue(TEXT("create_ik_rig handler found"),
        InvokeHandlerWithCapture(TEXT("animation.authoring.create_ik_rig"), CreatePayload, CreateCapture));
    TestTrue(TEXT("create_ik_rig succeeds (module compiled in)"),
        CreateCapture.bWasCalled && CreateCapture.bSuccess);

    // Resolve the rig from the handler's returned assetPath, not a reconstructed guess:
    // create_ik_rig routes through UIKRigDefinitionFactory::CreateNewIKRigAsset, whose
    // CreateUniqueAssetName can disambiguate the package name (e.g. a same-named in-memory
    // asset lingering from an earlier run in this editor session yields a "_1" suffix), so
    // GetPathName() is the source of truth — RigObjectPath is only the requested name.
    FString ActualRigObjectPath = RigObjectPath;
    if (CreateCapture.Result.IsValid())
    {
        CreateCapture.Result->TryGetStringField(TEXT("assetPath"), ActualRigObjectPath);
    }
    // The asset is created in memory (save=false) — find it directly rather than loading from
    // disk, where StaticLoadObject falls back to LoadPackage and reports SkipPackage.
    UIKRigDefinition* IKRig = FindObject<UIKRigDefinition>(nullptr, *ActualRigObjectPath);
    TestNotNull(TEXT("created IK Rig loads as a real UIKRigDefinition"), IKRig);
    if (!IKRig)
    {
        Mesh->RemoveFromRoot();
        Skeleton->RemoveFromRoot();
        return false;
    }

    // Load the rig's bone hierarchy from our transient mesh so AddRetargetChain can validate
    // the chain bones.
    UIKRigController* Controller = UIKRigController::GetController(IKRig);
    TestNotNull(TEXT("IK Rig controller available"), Controller);
    if (Controller)
    {
        Controller->SetSkeletalMesh(Mesh);
    }

    const int32 ChainsBefore = IKRig->GetRetargetChains().Num();

    // add_ik_chain via the production handler — must really push a chain onto the asset.
    TSharedPtr<FJsonObject> ChainPayload = MakeShared<FJsonObject>();
    ChainPayload->SetStringField(TEXT("assetPath"), ActualRigObjectPath);
    ChainPayload->SetStringField(TEXT("chainName"), TEXT("TestChain"));
    ChainPayload->SetStringField(TEXT("startBone"), TEXT("bone_a"));
    ChainPayload->SetStringField(TEXT("endBone"), TEXT("bone_b"));

    FTestResponseCapture ChainCapture;
    TestTrue(TEXT("add_ik_chain handler found"),
        InvokeHandlerWithCapture(TEXT("animation.authoring.add_ik_chain"), ChainPayload, ChainCapture));
    TestTrue(TEXT("add_ik_chain succeeds with valid bones"),
        ChainCapture.bWasCalled && ChainCapture.bSuccess);

    // The real mutation landed: GetRetargetChains grew and now contains our chain. This is the
    // assertion that fails if add_ik_chain regresses to the echo stub.
    const TArray<FBoneChain>& Chains = IKRig->GetRetargetChains();
    TestEqual(TEXT("retarget chain count incremented"), Chains.Num(), ChainsBefore + 1);
    bool bFoundChain = false;
    for (const FBoneChain& Chain : Chains)
    {
        if (Chain.ChainName == FName(TEXT("TestChain")))
        {
            bFoundChain = true;
            TestEqual(TEXT("chain start bone persisted"), Chain.StartBone.BoneName, FName(TEXT("bone_a")));
            TestEqual(TEXT("chain end bone persisted"), Chain.EndBone.BoneName, FName(TEXT("bone_b")));
            break;
        }
    }
    TestTrue(TEXT("added chain is present on the IK Rig asset"), bFoundChain);

    Mesh->RemoveFromRoot();
    Skeleton->RemoveFromRoot();
    FString ActualRigPackagePath = ActualRigObjectPath;
    int32 ActualDotIndex = INDEX_NONE;
    if (ActualRigPackagePath.FindChar(TEXT('.'), ActualDotIndex))
    {
        ActualRigPackagePath.LeftInline(ActualDotIndex);
    }
    CleanupTestAsset(ActualRigPackagePath);
    return true;
}
#endif // MCP_TEST_HAS_IKRIG
