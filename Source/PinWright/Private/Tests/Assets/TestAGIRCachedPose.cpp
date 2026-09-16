// Copyright (c) 2026 Alexander Penkin. MIT License.

// Wave 2 chunk wave-2-cached-pose regression test.
//
// Builds a synthetic source AnimBP with a SequencePlayer -> SaveCachedPose
// (CacheName="MainPose") and a UseCachedPose (NameOfCache="MainPose") wired
// into the root pose. Decompiles, compiles into a fresh target, and asserts
// the cross-reference (UseNode->SaveCachedPoseNode weak ptr) resolves at AGIR
// compile time — before the engine's EarlyValidation runs. Counterfactual:
// removing CacheNameMap.Add from the save handler leaves the use node's weak
// ptr null and the IsValid() assertion fails.
#include "Misc/AutomationTest.h"


#include "AGIR/AGIRCompiler.h"
#include "AGIR/AGIRDecompiler.h"
#include "AnimGraphNode_Base.h"
#include "AnimGraphNode_Root.h"
#include "AnimGraphNode_SaveCachedPose.h"
#include "AnimGraphNode_SequencePlayer.h"
#include "AnimGraphNode_UseCachedPose.h"
#include "AnimationGraph.h"
#include "Animation/AnimBlueprint.h"
#include "Animation/AnimInstance.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "EdGraph/EdGraph.h"
#include "Factories/AnimBlueprintFactory.h"
#include "Handlers/Animation/AnimGraphConstructionUtils.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Misc/Guid.h"
#include "Misc/ScopeExit.h"
#include "Tests/TestUtils.h"
#include "Tests/TestSkipReporting.h"
#include "UObject/Package.h"

namespace
{
const TCHAR* LyraMannequinAnimBPPath_CachedPose =
    TEXT("/Game/Characters/Mannequins/Animations/ABP_Manny.ABP_Manny");

UAnimBlueprint* LoadLyraMannequinAnimBPForCachedPose()
{
    return LoadObject<UAnimBlueprint>(nullptr, LyraMannequinAnimBPPath_CachedPose);
}

// Mirrors CreateFreshAnimBlueprint in TestAnimGraphHandlers.cpp; redeclared
// locally to keep this test file self-contained.
UAnimBlueprint* CreateFreshAnimBlueprintForCachedPose(const FString& PackagePath, USkeleton* Skeleton)
{
    FString FolderPath;
    FString AssetName;
    PackagePath.Split(TEXT("/"), &FolderPath, &AssetName, ESearchCase::CaseSensitive, ESearchDir::FromEnd);
    UPackage* Package = CreatePackage(*PackagePath);
    if (!Package)
    {
        return nullptr;
    }

    UAnimBlueprintFactory* Factory = NewObject<UAnimBlueprintFactory>();
    Factory->TargetSkeleton = Skeleton;
    Factory->ParentClass = UAnimInstance::StaticClass();
    UAnimBlueprint* NewBP = Cast<UAnimBlueprint>(
        Factory->FactoryCreateNew(UAnimBlueprint::StaticClass(), Package,
            FName(*AssetName), RF_Public | RF_Standalone, nullptr, GWarn));
    if (!NewBP)
    {
        return nullptr;
    }

    FAssetRegistryModule::AssetCreated(NewBP);
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(NewBP);
    return NewBP;
}

UAnimGraphNode_Root* FindRootInGraph(UEdGraph* Graph)
{
    if (!Graph)
    {
        return nullptr;
    }
    for (UEdGraphNode* Node : Graph->Nodes)
    {
        if (UAnimGraphNode_Root* Root = Cast<UAnimGraphNode_Root>(Node))
        {
            return Root;
        }
    }
    return nullptr;
}

template <typename TNode>
TNode* FindFirstOfClass(UEdGraph* Graph)
{
    if (!Graph)
    {
        return nullptr;
    }
    for (UEdGraphNode* Node : Graph->Nodes)
    {
        if (TNode* Match = Cast<TNode>(Node))
        {
            return Match;
        }
    }
    return nullptr;
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAGIRCachedPoseRoundTripTest,
    "PinWright.AGIR.CachedPose.RoundTrip",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAGIRCachedPoseRoundTripTest::RunTest(const FString& Parameters)
{
    PINWRIGHT_SKIP_IF_FIXTURE_MISSING(LyraMannequinAnimBPPath_CachedPose);
    UAnimBlueprint* Fixture = LoadLyraMannequinAnimBPForCachedPose();
    if (!TestNotNull(TEXT("Mannequin AnimBP fixture loaded"), Fixture)) return false;
    if (!TestNotNull(TEXT("fixture has TargetSkeleton"), Fixture->TargetSkeleton.Get())) return false;
    USkeleton* Skeleton = Fixture->TargetSkeleton;

    const FString Guid = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString SourcePath = FString::Printf(
        TEXT("/Game/PinWrightTests/ABP_AGIR_CP_Src_%s"), *Guid);
    const FString TargetPath = FString::Printf(
        TEXT("/Game/PinWrightTests/ABP_AGIR_CP_%s"), *Guid);

    ON_SCOPE_EXIT
    {
        CleanupTestAsset(SourcePath);
        CleanupTestAsset(TargetPath);
    };

    UAnimBlueprint* SourceBP = CreateFreshAnimBlueprintForCachedPose(SourcePath, Skeleton);
    if (!SourceBP)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-unavailable"),
            TEXT("CachedPoseRoundTrip: could not create source AnimBlueprint - skipped."));
        return true;
    }

    UEdGraph* SourceAnimGraph = AnimGraphConstructionUtils::GetAnimGraphFromBlueprint(SourceBP);
    if (!SourceAnimGraph)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-unavailable"),
            TEXT("CachedPoseRoundTrip: source AnimBP missing default AnimGraph page - skipped."));
        return true;
    }

    // Source layout:
    //   SequencePlayer ----Pose---> SaveCachedPose(CacheName="MainPose")
    //                              UseCachedPose(NameOfCache="MainPose") --> Root.Result
    UAnimGraphNode_Base* SeqNode = AnimGraphConstructionUtils::CreateAnimNode(
        SourceAnimGraph, UAnimGraphNode_SequencePlayer::StaticClass(), FVector2D(0, 0));
    UAnimGraphNode_Base* SaveBase = AnimGraphConstructionUtils::CreateAnimNode(
        SourceAnimGraph, UAnimGraphNode_SaveCachedPose::StaticClass(), FVector2D(300, 0));
    UAnimGraphNode_SaveCachedPose* SaveNode = Cast<UAnimGraphNode_SaveCachedPose>(SaveBase);
    UAnimGraphNode_Base* UseBase = AnimGraphConstructionUtils::CreateAnimNode(
        SourceAnimGraph, UAnimGraphNode_UseCachedPose::StaticClass(), FVector2D(600, 0));
    UAnimGraphNode_UseCachedPose* UseNode = Cast<UAnimGraphNode_UseCachedPose>(UseBase);
    UAnimGraphNode_Root* RootNode = FindRootInGraph(SourceAnimGraph);

    TestNotNull(TEXT("SequencePlayer created"), SeqNode);
    TestNotNull(TEXT("SaveCachedPose created"), SaveNode);
    TestNotNull(TEXT("UseCachedPose created"), UseNode);
    TestNotNull(TEXT("source AnimGraph default Root node present"), RootNode);
    if (!SeqNode || !SaveNode || !UseNode || !RootNode)
    {
        return false;
    }

    SaveNode->CacheName = TEXT("MainPose");
    UseNode->SaveCachedPoseNode = SaveNode;

    // Wire SequencePlayer.Pose -> SaveCachedPose.Pose, UseCachedPose.Pose ->
    // Root.Result. The save node has a single FPoseLink runtime field named
    // "Pose"; the upstream output pose pin on every UAnimGraphNode_Base is
    // also "Pose".
    const bool bWiredSeqToSave = AnimGraphConstructionUtils::WirePoseLink(
        SeqNode, FName(TEXT("Pose")), SaveNode, FName(TEXT("Pose")));
    const bool bWiredUseToRoot = AnimGraphConstructionUtils::WirePoseLink(
        UseNode, FName(TEXT("Pose")), RootNode, FName(TEXT("Result")));
    TestTrue(TEXT("wired SequencePlayer -> SaveCachedPose"), bWiredSeqToSave);
    TestTrue(TEXT("wired UseCachedPose -> Root"), bWiredUseToRoot);

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(SourceBP);

    // Decompile source -> AGIR text.
    FAGIRDecompileResult DecompileResult = FAGIRDecompiler(SourceBP).Decompile();
    TestTrue(TEXT("decompile of source succeeds"), DecompileResult.bSuccess);
    if (!DecompileResult.bSuccess || DecompileResult.AGIRText.IsEmpty())
    {
        return false;
    }

    // Sanity: AGIR text mentions both opcodes and the cache name token.
    TestTrue(TEXT("AGIR text contains save_cached_pose"),
        DecompileResult.AGIRText.Contains(TEXT("save_cached_pose")));
    TestTrue(TEXT("AGIR text contains use_cached_pose"),
        DecompileResult.AGIRText.Contains(TEXT("use_cached_pose")));
    TestTrue(TEXT("AGIR text contains cache name 'MainPose'"),
        DecompileResult.AGIRText.Contains(TEXT("MainPose")));

    // Compile -> fresh target.
    UAnimBlueprint* TargetBP = CreateFreshAnimBlueprintForCachedPose(TargetPath, Skeleton);
    TestNotNull(TEXT("target AnimBP created"), TargetBP);
    if (!TargetBP)
    {
        return false;
    }

    FAGIRCompileOptions Options;
    Options.Context = TargetBP->GetPathName();
    Options.Mode = EAGIRCompileMode::Replace;
    Options.bRunLayout = false;
    Options.bSave = false;

    FAGIRCompileResult CompileResult = FAGIRCompiler::Compile(DecompileResult.AGIRText, Options);
    TestTrue(FString::Printf(TEXT("compile to target succeeds (errorCode='%s', message='%s')"),
        *CompileResult.ErrorCode, *CompileResult.ErrorMessage), CompileResult.bSuccess);
    if (!CompileResult.bSuccess)
    {
        return false;
    }

    UEdGraph* TargetAnimGraph = AnimGraphConstructionUtils::GetAnimGraphFromBlueprint(TargetBP);
    TestNotNull(TEXT("target AnimGraph present"), TargetAnimGraph);
    if (!TargetAnimGraph)
    {
        return false;
    }

    UAnimGraphNode_SaveCachedPose* TargetSave =
        FindFirstOfClass<UAnimGraphNode_SaveCachedPose>(TargetAnimGraph);
    UAnimGraphNode_UseCachedPose* TargetUse =
        FindFirstOfClass<UAnimGraphNode_UseCachedPose>(TargetAnimGraph);
    TestNotNull(TEXT("target has a SaveCachedPose node"), TargetSave);
    TestNotNull(TEXT("target has a UseCachedPose node"), TargetUse);
    if (!TargetSave || !TargetUse)
    {
        return false;
    }

    TestEqual(TEXT("SaveCachedPose CacheName round-trips"),
        TargetSave->CacheName, FString(TEXT("MainPose")));

    // Counterfactual: removing CacheNameMap.Add from CompileSaveCachedPoseInstruction
    // leaves SaveCachedPoseNode null because CompileUseCachedPoseInstruction
    // can't resolve "MainPose" against the empty map. The engine's
    // EarlyValidation would fix it on AnimBP compile, but this assertion fires
    // immediately after AGIR compile.
    TestTrue(TEXT("UseCachedPose SaveCachedPoseNode weak-ptr resolved at AGIR compile time"),
        TargetUse->SaveCachedPoseNode.IsValid());
    if (TargetUse->SaveCachedPoseNode.IsValid())
    {
        TestEqual(TEXT("UseCachedPose linked back to the correct Save node by CacheName"),
            TargetUse->SaveCachedPoseNode->CacheName, FString(TEXT("MainPose")));
    }
    return true;
}
