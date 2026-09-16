// Copyright (c) 2026 Alexander Penkin. MIT License.

// Wave 2 chunk wave-2-layered-blend regression test.
//
// Round-trip a synthetic source AnimBP that contains a UAnimGraphNode_LayeredBoneBlend
// with three poses, each wired to a distinct UAnimGraphNode_SequencePlayer.
// Decompile -> compile into a fresh target AnimBP and assert structural fidelity:
// the target's main AnimGraph contains a layered_blend node with BlendPoses.Num() == 3
// and three SequencePlayer nodes wired into the per-index pose pins.
//
// Counterfactual: reverting CompileLayeredBlendInstruction to the Wave-1 stub fails
// the bSuccess assertion (returns AGIR_SUBGRAPH_NOT_SUPPORTED).
#include "Misc/AutomationTest.h"


#include "AGIR/AGIRCompiler.h"
#include "AGIR/AGIRDecompiler.h"
#include "AnimGraphNode_Base.h"
#include "AnimGraphNode_LayeredBoneBlend.h"
#include "AnimGraphNode_SequencePlayer.h"
#include "AnimNodes/AnimNode_LayeredBoneBlend.h"
#include "Animation/AnimBlueprint.h"
#include "Animation/AnimInstance.h"
#include "AnimationGraph.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
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
const TCHAR* LyraMannequinAnimBPPath_LayeredBlend =
    TEXT("/Game/Characters/Mannequins/Animations/ABP_Manny.ABP_Manny");

UAnimBlueprint* LoadLyraMannequinAnimBPForLayeredBlend()
{
    return LoadObject<UAnimBlueprint>(nullptr, LyraMannequinAnimBPPath_LayeredBlend);
}

UAnimBlueprint* CreateFreshAnimBlueprintForLayeredBlend(const FString& PackagePath, USkeleton* Skeleton)
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

// Walk the supplied anim graph, returning the first UAnimGraphNode_LayeredBoneBlend.
UAnimGraphNode_LayeredBoneBlend* FindLayeredBoneBlendNode(UEdGraph* Graph)
{
    if (!Graph)
    {
        return nullptr;
    }
    for (UEdGraphNode* Node : Graph->Nodes)
    {
        if (UAnimGraphNode_LayeredBoneBlend* Layered = Cast<UAnimGraphNode_LayeredBoneBlend>(Node))
        {
            return Layered;
        }
    }
    return nullptr;
}

// Count how many of the layered blend's `BlendPoses_<i>` input pins resolve to a
// UAnimGraphNode_SequencePlayer upstream. Used to confirm the round-trip wired
// each pose connection back to a sequence player on the target.
int32 CountSequencePlayerPosesWired(UAnimGraphNode_LayeredBoneBlend* Layered)
{
    if (!Layered)
    {
        return 0;
    }
    int32 Count = 0;
    for (UEdGraphPin* Pin : Layered->Pins)
    {
        if (!Pin || Pin->Direction != EGPD_Input)
        {
            continue;
        }
        const FString PinName = Pin->PinName.ToString();
        if (!PinName.StartsWith(TEXT("BlendPoses_")))
        {
            continue;
        }
        if (Pin->LinkedTo.Num() == 0 || !Pin->LinkedTo[0])
        {
            continue;
        }
        UEdGraphNode* Upstream = Pin->LinkedTo[0]->GetOwningNode();
        if (Cast<UAnimGraphNode_SequencePlayer>(Upstream))
        {
            ++Count;
        }
    }
    return Count;
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAGIRLayeredBlendRoundTripTest,
    "PinWright.AGIR.LayeredBlend.RoundTrip",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAGIRLayeredBlendRoundTripTest::RunTest(const FString& Parameters)
{
    PINWRIGHT_SKIP_IF_FIXTURE_MISSING(LyraMannequinAnimBPPath_LayeredBlend);
    UAnimBlueprint* Fixture = LoadLyraMannequinAnimBPForLayeredBlend();
    if (!TestNotNull(TEXT("Mannequin AnimBP fixture loaded"), Fixture)) return false;
    if (!TestNotNull(TEXT("fixture has TargetSkeleton"), Fixture->TargetSkeleton.Get())) return false;
    USkeleton* Skeleton = Fixture->TargetSkeleton;

    const FString Guid = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString SourcePath = FString::Printf(
        TEXT("/Game/PinWrightTests/ABP_AGIR_LayeredBlendSrc_%s"), *Guid);
    const FString TargetPath = FString::Printf(
        TEXT("/Game/PinWrightTests/ABP_AGIR_LayeredBlendTgt_%s"), *Guid);

    ON_SCOPE_EXIT
    {
        CleanupTestAsset(SourcePath);
        CleanupTestAsset(TargetPath);
    };

    UAnimBlueprint* SourceBP = CreateFreshAnimBlueprintForLayeredBlend(SourcePath, Skeleton);
    if (!SourceBP)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-unavailable"),
            TEXT("LayeredBlendRoundTrip: could not create source AnimBlueprint - skipped."));
        return true;
    }

    UEdGraph* SourceAnimGraph = AnimGraphConstructionUtils::GetAnimGraphFromBlueprint(SourceBP);
    TestNotNull(TEXT("source AnimGraph present"), SourceAnimGraph);
    if (!SourceAnimGraph)
    {
        return false;
    }

    // Author the source: a LayeredBoneBlend with three poses (constructor seeds
    // the first via AddFirstPose, so call AddPose() twice more) wired to three
    // SequencePlayer nodes via the BlendPoses_<i> editor pins.
    UAnimGraphNode_Base* LayeredBase = AnimGraphConstructionUtils::CreateAnimNode(
        SourceAnimGraph, UAnimGraphNode_LayeredBoneBlend::StaticClass(), FVector2D(300, 0));
    UAnimGraphNode_LayeredBoneBlend* SourceLayered = Cast<UAnimGraphNode_LayeredBoneBlend>(LayeredBase);
    TestNotNull(TEXT("source LayeredBoneBlend created"), SourceLayered);
    if (!SourceLayered)
    {
        return false;
    }
    SourceLayered->Node.AddPose();
    SourceLayered->Node.AddPose();
    SourceLayered->ReconstructNode();
    TestEqual(TEXT("source LayeredBoneBlend has 3 poses"),
        SourceLayered->Node.BlendPoses.Num(), 3);

    UAnimGraphNode_Base* SeqA = AnimGraphConstructionUtils::CreateAnimNode(
        SourceAnimGraph, UAnimGraphNode_SequencePlayer::StaticClass(), FVector2D(0, 0));
    UAnimGraphNode_Base* SeqB = AnimGraphConstructionUtils::CreateAnimNode(
        SourceAnimGraph, UAnimGraphNode_SequencePlayer::StaticClass(), FVector2D(0, 200));
    UAnimGraphNode_Base* SeqC = AnimGraphConstructionUtils::CreateAnimNode(
        SourceAnimGraph, UAnimGraphNode_SequencePlayer::StaticClass(), FVector2D(0, 400));
    if (!SeqA || !SeqB || !SeqC)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-unavailable"),
            TEXT("LayeredBlendRoundTrip: could not author source SequencePlayers - skipped."));
        return true;
    }

    AnimGraphConstructionUtils::WirePoseLink(SeqA, FName(TEXT("Pose")),
        SourceLayered, FName(TEXT("BlendPoses_0")));
    AnimGraphConstructionUtils::WirePoseLink(SeqB, FName(TEXT("Pose")),
        SourceLayered, FName(TEXT("BlendPoses_1")));
    AnimGraphConstructionUtils::WirePoseLink(SeqC, FName(TEXT("Pose")),
        SourceLayered, FName(TEXT("BlendPoses_2")));

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(SourceBP);

    // Decompile source -> AGIR text.
    FAGIRDecompileResult DecompileResult = FAGIRDecompiler(SourceBP).Decompile();
    TestTrue(TEXT("decompile of source succeeds"), DecompileResult.bSuccess);
    if (!DecompileResult.bSuccess || DecompileResult.AGIRText.IsEmpty())
    {
        AddError(FString::Printf(TEXT("decompile produced no text (warnings: %d)"),
            DecompileResult.Warnings.Num()));
        return false;
    }

    // Compile -> fresh target.
    UAnimBlueprint* TargetBP = CreateFreshAnimBlueprintForLayeredBlend(TargetPath, Skeleton);
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

    UAnimGraphNode_LayeredBoneBlend* TargetLayered = FindLayeredBoneBlendNode(TargetAnimGraph);
    TestNotNull(TEXT("target contains a LayeredBoneBlend node"), TargetLayered);
    if (!TargetLayered)
    {
        return false;
    }

    TestEqual(TEXT("target LayeredBoneBlend has 3 poses"),
        TargetLayered->Node.BlendPoses.Num(), 3);

    const int32 WiredPoses = CountSequencePlayerPosesWired(TargetLayered);
    TestEqual(TEXT("all three BlendPoses_<i> pins wired to SequencePlayers"),
        WiredPoses, 3);

    return true;
}
