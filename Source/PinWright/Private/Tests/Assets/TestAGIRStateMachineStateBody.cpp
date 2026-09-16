// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"

#include "AGIR/AGIRDecompiler.h"
#include "AnimGraphNode_Base.h"
#include "AnimGraphNode_SequencePlayer.h"
#include "AnimGraphNode_StateMachine.h"
#include "AnimGraphNode_StateResult.h"
#include "AnimStateNode.h"
#include "Animation/AnimBlueprint.h"
#include "Animation/AnimSequence.h"
#include "Animation/Skeleton.h"
#include "AnimationStateGraph.h"
#include "AnimationStateMachineGraph.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "EdGraph/EdGraph.h"
#include "Handlers/Animation/AnimGraphConstructionUtils.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Misc/Guid.h"
#include "Misc/ScopeExit.h"
#include "TestAGIRFixtures.h"
#include "Tests/TestUtils.h"
#include "UObject/Package.h"

namespace
{
UAnimSequence* CreateScratchAnimSequenceForStateBody(const FString& PackagePath, USkeleton* Skeleton)
{
    FString FolderPath;
    FString AssetName;
    PackagePath.Split(TEXT("/"), &FolderPath, &AssetName, ESearchCase::CaseSensitive, ESearchDir::FromEnd);
    UPackage* Package = CreatePackage(*PackagePath);
    if (!Package)
    {
        return nullptr;
    }

    UAnimSequence* Sequence = NewObject<UAnimSequence>(
        Package, FName(*AssetName), RF_Public | RF_Standalone);
    if (!Sequence)
    {
        return nullptr;
    }

    Sequence->SetSkeleton(Skeleton);
    FAssetRegistryModule::AssetCreated(Sequence);
    Package->MarkPackageDirty();
    return Sequence;
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAGIRStateMachineStateBodyEmitsSequenceAssetRefsTest,
    "PinWright.agir.StateMachineStateBodyEmitsSequenceAssetRefs",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAGIRStateMachineStateBodyEmitsSequenceAssetRefsTest::RunTest(const FString& Parameters)
{
    // Host Mannequin AnimBP (shared path constant from TestAGIRFixtures.h) —
    // borrowed only for its real skeleton. Host absence skips; a package that
    // exists but fails to load is still a hard failure.
    PINWRIGHT_SKIP_IF_FIXTURE_MISSING(AGIRTestFixtures::LyraMannequinAnimBPPath);
    UAnimBlueprint* Fixture = LoadObject<UAnimBlueprint>(nullptr,
        AGIRTestFixtures::LyraMannequinAnimBPPath);
    if (!TestNotNull(TEXT("Mannequin AnimBP fixture loaded"), Fixture)) return false;
    if (!TestNotNull(TEXT("fixture has TargetSkeleton"), Fixture->TargetSkeleton.Get())) return false;
    USkeleton* Skeleton = Fixture->TargetSkeleton;

    const FString Guid = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString BlueprintPath = FString::Printf(
        TEXT("/Game/PinWrightTests/ABP_AGIR_StateBody_%s"), *Guid);
    const FString SequencePath = FString::Printf(
        TEXT("/Game/PinWrightTests/AS_AGIR_StateBody_%s"), *Guid);

    ON_SCOPE_EXIT
    {
        CleanupTestAsset(BlueprintPath);
        CleanupTestAsset(SequencePath);
    };

    UAnimBlueprint* AnimBP = AGIRTestFixtures::CreateFreshAnimBlueprint(BlueprintPath, Skeleton);
    UAnimSequence* Sequence = CreateScratchAnimSequenceForStateBody(SequencePath, Skeleton);
    TestNotNull(TEXT("scratch AnimBlueprint created"), AnimBP);
    TestNotNull(TEXT("scratch AnimSequence created"), Sequence);
    if (!AnimBP || !Sequence)
    {
        return false;
    }

    UEdGraph* AnimGraph = AnimGraphConstructionUtils::GetAnimGraphFromBlueprint(AnimBP);
    TestNotNull(TEXT("AnimGraph present"), AnimGraph);
    if (!AnimGraph)
    {
        return false;
    }

    UAnimGraphNode_StateMachine* StateMachine = AnimGraphConstructionUtils::CreateStateMachine(
        AnimBP, AnimGraph, FName(TEXT("LocomotionSM")), FVector2D(0, 0));
    TestNotNull(TEXT("state machine created"), StateMachine);
    if (!StateMachine || !StateMachine->EditorStateMachineGraph)
    {
        return false;
    }

    UAnimationStateMachineGraph* MachineGraph =
        Cast<UAnimationStateMachineGraph>(StateMachine->EditorStateMachineGraph);
    TestNotNull(TEXT("state machine graph castable"), MachineGraph);
    if (!MachineGraph)
    {
        return false;
    }

    UAnimStateNode* StateNode = AnimGraphConstructionUtils::CreateState(
        MachineGraph, FName(TEXT("Idle")), FVector2D(200, 0));
    TestNotNull(TEXT("state node created"), StateNode);
    if (!StateNode || !StateNode->BoundGraph)
    {
        return false;
    }

    UAnimationStateGraph* StateGraph = Cast<UAnimationStateGraph>(StateNode->BoundGraph);
    UAnimGraphNode_StateResult* StateResult = StateGraph ? StateGraph->GetResultNode() : nullptr;
    TestNotNull(TEXT("state result node present"), StateResult);
    if (!StateGraph || !StateResult)
    {
        return false;
    }

    UAnimGraphNode_SequencePlayer* SequencePlayer = Cast<UAnimGraphNode_SequencePlayer>(
        AnimGraphConstructionUtils::CreateAnimNode(
            StateGraph, UAnimGraphNode_SequencePlayer::StaticClass(), FVector2D(0, 0)));
    TestNotNull(TEXT("SequencePlayer node created in state body"), SequencePlayer);
    if (!SequencePlayer)
    {
        return false;
    }

    SequencePlayer->Node.SetSequence(Sequence);
    SequencePlayer->Node.SetPlayRate(1.25f);
    SequencePlayer->Node.SetGroupName(FName(TEXT("Locomotion")));
    SequencePlayer->Node.SetGroupMethod(EAnimSyncMethod::SyncGroup);

    const bool bWiredSequenceToStateResult = AnimGraphConstructionUtils::WirePoseLink(
        SequencePlayer, FName(TEXT("Pose")), StateResult, FName(TEXT("Result")));
    TestTrue(TEXT("wired SequencePlayer -> StateResult"), bWiredSequenceToStateResult);
    if (!bWiredSequenceToStateResult)
    {
        return false;
    }

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(AnimBP);

    FAGIRDecompileResult DecompileResult = FAGIRDecompiler(AnimBP).Decompile();
    TestTrue(TEXT("decompile reports success"), DecompileResult.bSuccess);
    if (!DecompileResult.bSuccess)
    {
        return false;
    }

    const FString SequenceObjectPath = Sequence->GetPathName();
    TestTrue(TEXT("AGIR contains state block"),
        DecompileResult.AGIRText.Contains(TEXT("state Idle {")));
    TestTrue(TEXT("AGIR contains SequencePlayer call"),
        DecompileResult.AGIRText.Contains(TEXT("AnimGraphNode_SequencePlayer")));
    TestTrue(TEXT("AGIR contains Sequence field"),
        DecompileResult.AGIRText.Contains(TEXT("Sequence:")));
    TestTrue(TEXT("AGIR contains sequence object path"),
        DecompileResult.AGIRText.Contains(SequenceObjectPath));
    TestTrue(TEXT("AGIR contains PlayRate field"),
        DecompileResult.AGIRText.Contains(TEXT("PlayRate: 1.25")));
    TestTrue(TEXT("AGIR contains GroupName field"),
        DecompileResult.AGIRText.Contains(TEXT("GroupName: \"Locomotion\"")));
    TestTrue(TEXT("AGIR contains state body output"),
        DecompileResult.AGIRText.Contains(TEXT("output %sequence_player_")));

    return true;
}
