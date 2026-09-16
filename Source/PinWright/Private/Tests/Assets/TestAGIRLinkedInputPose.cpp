// Copyright (c) 2026 Alexander Penkin. MIT License.

// Wave 2 chunk wave-2-linked-input-pose regression test.
//
// `CompileLinkedInputPoseInstruction` upserts UAnimGraphNode_LinkedInputPose
// nodes by runtime Name so a target function graph that already carries a
// schema-seeded LinkedInputPose does not end up with duplicates after compile.
// The test seeds a synthetic source AnimBP with one anim function graph that
// hosts a LinkedInputPose, decompiles, then compiles into a fresh target
// AnimBP whose matching function graph already contains its own seeded
// LinkedInputPose.
#include "Misc/AutomationTest.h"


#include "AGIR/AGIRCompiler.h"
#include "AGIR/AGIRDecompiler.h"
#include "AnimGraphNode_LinkedInputPose.h"
#include "AnimationGraph.h"
#include "AnimationGraphSchema.h"
#include "Animation/AnimBlueprint.h"
#include "Animation/AnimInstance.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraphSchema_K2.h"
#include "Factories/AnimBlueprintFactory.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Misc/Guid.h"
#include "Misc/ScopeExit.h"
#include "Tests/TestUtils.h"
#include "UObject/Package.h"

namespace
{
// Host-project Mannequin AnimBP (identifier keeps the "Lyra" name
// for parity with sibling AGIR tests). Used only for its TargetSkeleton — this
// test does NOT depend on any anim layer interface.
const TCHAR* LyraMannequinAnimBPPath_LinkedInputPose =
    TEXT("/Game/Characters/Mannequins/Animations/ABP_Manny.ABP_Manny");

UAnimBlueprint* LoadLyraMannequinAnimBPForLinkedInputPose()
{
    return LoadObject<UAnimBlueprint>(nullptr, LyraMannequinAnimBPPath_LinkedInputPose);
}

// Mirrors CreateFreshAnimBlueprint in TestAnimGraphHandlers.cpp; redeclared
// locally so the test is self-contained.
UAnimBlueprint* CreateFreshAnimBlueprintForLinkedInputPose(const FString& PackagePath, USkeleton* Skeleton)
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

// Adds a UAnimationGraph function graph named `GraphName` to the AnimBP and
// places a single LinkedInputPose into it. Returns the graph so the caller can
// run decompile/compile assertions against it.
UEdGraph* SeedAnimFunctionGraphWithLinkedInputPose(
    UAnimBlueprint* AnimBP, FName GraphName, FName PoseName)
{
    if (!AnimBP)
    {
        return nullptr;
    }
    UEdGraph* FuncGraph = FBlueprintEditorUtils::CreateNewGraph(
        AnimBP, GraphName, UAnimationGraph::StaticClass(), UAnimationGraphSchema::StaticClass());
    if (!FuncGraph)
    {
        return nullptr;
    }
    AnimBP->FunctionGraphs.Add(FuncGraph);

    FGraphNodeCreator<UAnimGraphNode_LinkedInputPose> NodeCreator(*FuncGraph);
    UAnimGraphNode_LinkedInputPose* LinkedInputNode = NodeCreator.CreateNode(/*bSelectNewNode=*/false);
    if (!LinkedInputNode)
    {
        return nullptr;
    }
    LinkedInputNode->Node.Name = PoseName;
    LinkedInputNode->InputPoseIndex = 0;
    NodeCreator.Finalize();

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(AnimBP);
    return FuncGraph;
}

int32 CountLinkedInputPoseNodes(UEdGraph* Graph)
{
    if (!Graph)
    {
        return 0;
    }
    int32 Count = 0;
    for (UEdGraphNode* Node : Graph->Nodes)
    {
        if (Cast<UAnimGraphNode_LinkedInputPose>(Node) != nullptr)
        {
            ++Count;
        }
    }
    return Count;
}

UAnimGraphNode_LinkedInputPose* FindLinkedInputPoseByName(UEdGraph* Graph, FName Name)
{
    if (!Graph)
    {
        return nullptr;
    }
    for (UEdGraphNode* Node : Graph->Nodes)
    {
        UAnimGraphNode_LinkedInputPose* Cast_Node = Cast<UAnimGraphNode_LinkedInputPose>(Node);
        if (Cast_Node && Cast_Node->Node.Name == Name)
        {
            return Cast_Node;
        }
    }
    return nullptr;
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAGIRLinkedInputPoseRoundTripTest,
    "PinWright.AGIR.LinkedInputPose.RoundTrip",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAGIRLinkedInputPoseRoundTripTest::RunTest(const FString& Parameters)
{
    PINWRIGHT_SKIP_IF_FIXTURE_MISSING(LyraMannequinAnimBPPath_LinkedInputPose);
    UAnimBlueprint* Fixture = LoadLyraMannequinAnimBPForLinkedInputPose();
    if (!TestNotNull(TEXT("host Mannequin AnimBP loadable"), Fixture))
    {
        return false;
    }
    USkeleton* Skeleton = Fixture->TargetSkeleton;
    if (!TestNotNull(TEXT("host Mannequin AnimBP has TargetSkeleton"), Skeleton))
    {
        return false;
    }

    const FString Guid = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString SourcePath = FString::Printf(
        TEXT("/Game/PinWrightTests/ABP_AGIRLinkedInputPose_Src_%s"), *Guid);
    const FString TargetPath = FString::Printf(
        TEXT("/Game/PinWrightTests/ABP_AGIRLinkedInputPose_Tgt_%s"), *Guid);

    ON_SCOPE_EXIT
    {
        CleanupTestAsset(SourcePath);
        CleanupTestAsset(TargetPath);
    };

    UAnimBlueprint* SourceBP = CreateFreshAnimBlueprintForLinkedInputPose(SourcePath, Skeleton);
    if (!TestNotNull(TEXT("source AnimBlueprint created"), SourceBP))
    {
        return false;
    }

    const FName GraphName(TEXT("MyAnimFunc"));
    const FName PoseName(TEXT("InPose"));
    UEdGraph* SourceFuncGraph = SeedAnimFunctionGraphWithLinkedInputPose(SourceBP, GraphName, PoseName);
    TestNotNull(TEXT("source function graph created"), SourceFuncGraph);
    if (!SourceFuncGraph)
    {
        return false;
    }
    TestEqual(TEXT("source has exactly one LinkedInputPose"),
        CountLinkedInputPoseNodes(SourceFuncGraph), 1);

    // Push one entry into SourceNode->Inputs so the post-round-trip
    // Inputs.Num() assertion is load-bearing (default-constructed Inputs is
    // empty, which would let the assertion pass even if Inputs were dropped).
    UAnimGraphNode_LinkedInputPose* SourceNode = FindLinkedInputPoseByName(SourceFuncGraph, PoseName);
    TestNotNull(TEXT("source LinkedInputPose retrievable post-seed"), SourceNode);
    if (!SourceNode)
    {
        return false;
    }
    {
        FEdGraphPinType BoolPinType;
        BoolPinType.PinCategory = UEdGraphSchema_K2::PC_Boolean;
        SourceNode->Inputs.Add(FAnimBlueprintFunctionPinInfo(FName(TEXT("ExtraInput")), BoolPinType));
    }

    FAGIRDecompileResult DecompileResult = FAGIRDecompiler(SourceBP).Decompile();
    TestTrue(TEXT("decompile succeeds"), DecompileResult.bSuccess);
    if (!DecompileResult.bSuccess || DecompileResult.AGIRText.IsEmpty())
    {
        return false;
    }
    TestTrue(TEXT("decompile classified MyAnimFunc as anim_function"),
        DecompileResult.AGIRText.Contains(
            TEXT("# ==== Graph: MyAnimFunc (anim_function) ====")));

    // Target: pre-seed the same-named function graph WITH a LinkedInputPose
    // node so the upsert path is exercised — naive create-only would result in
    // two LinkedInputPose nodes after compile.
    UAnimBlueprint* TargetBP = CreateFreshAnimBlueprintForLinkedInputPose(TargetPath, Skeleton);
    TestNotNull(TEXT("target AnimBP created"), TargetBP);
    if (!TargetBP)
    {
        return false;
    }
    UEdGraph* TargetFuncGraph = SeedAnimFunctionGraphWithLinkedInputPose(TargetBP, GraphName, PoseName);
    TestNotNull(TEXT("target function graph seeded"), TargetFuncGraph);
    if (!TargetFuncGraph)
    {
        return false;
    }
    TestEqual(TEXT("target seeded with one LinkedInputPose pre-compile"),
        CountLinkedInputPoseNodes(TargetFuncGraph), 1);

    FAGIRCompileOptions Options;
    Options.Context = TargetBP->GetPathName();
    // Replace mode clears authored nodes (LinkedInputPose included) before
    // compile, so the seeded node is removed and the AGIR-driven one created
    // fresh. Use Extend mode so the seeded node survives the pre-clear and
    // the upsert path is exercised.
    Options.Mode = EAGIRCompileMode::Extend;
    Options.bRunLayout = false;
    Options.bSave = false;

    FAGIRCompileResult CompileResult = FAGIRCompiler::Compile(DecompileResult.AGIRText, Options);
    TestTrue(FString::Printf(TEXT("compile succeeds (errorCode='%s', message='%s')"),
        *CompileResult.ErrorCode, *CompileResult.ErrorMessage), CompileResult.bSuccess);
    if (!CompileResult.bSuccess)
    {
        return false;
    }

    // Counterfactual: removing the upsert (find-by-Name) path produces TWO
    // LinkedInputPose nodes (the seeded one + the AGIR-created one), failing
    // this assertion.
    TestEqual(TEXT("target has exactly one LinkedInputPose post-compile"),
        CountLinkedInputPoseNodes(TargetFuncGraph), 1);

    UAnimGraphNode_LinkedInputPose* TargetNode = FindLinkedInputPoseByName(TargetFuncGraph, PoseName);
    TestNotNull(TEXT("target LinkedInputPose with same Name present"), TargetNode);
    if (TargetNode)
    {
        TestEqual(TEXT("target LinkedInputPose Name round-trips"),
            TargetNode->Node.Name, PoseName);
        TestEqual(TEXT("target LinkedInputPose InputPoseIndex round-trips"),
            TargetNode->InputPoseIndex, 0);
        TestEqual(TEXT("Inputs.Num round-trips"),
            TargetNode->Inputs.Num(), SourceNode->Inputs.Num());
    }

    return true;
}
