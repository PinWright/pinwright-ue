// Copyright (c) 2026 Alexander Penkin. MIT License.

// Wave 3 chunk wave-3-decompile-bodies regression test.
//
// Round-trip: build a source AnimBP carrying a blend_space node with a
// populated sample_graph by compiling hand-written AGIR text, then run
// FAGIRDecompiler.Decompile() to capture round-trip AGIR text, then compile
// that text into a fresh target AnimBP. Asserts structural equivalence
// (BlendSpaceGraph parent + sample sub-graph contents) survives the full
// compile→decompile→compile loop.
#include "Misc/AutomationTest.h"


#include "AGIR/AGIRCompiler.h"
#include "AGIR/AGIRDecompiler.h"
#include "AnimGraphNode_BlendSpaceGraph.h"
#include "AnimGraphNode_BlendSpaceGraphBase.h"
#include "AnimGraphNode_BlendSpaceSampleResult.h"
#include "AnimGraphNode_SequencePlayer.h"
#include "AnimationBlendSpaceSampleGraph.h"
#include "AnimationGraph.h"
#include "Animation/AnimBlueprint.h"
#include "Animation/AnimInstance.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "BlendSpaceGraph.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "Factories/AnimBlueprintFactory.h"
#include "Handlers/Animation/AnimGraphConstructionUtils.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Misc/Guid.h"
#include "Misc/ScopeExit.h"
#include "Tests/TestUtils.h"
#include "UObject/Package.h"

namespace
{
const TCHAR* LyraMannequinAnimBPPath_BlendSpace =
    TEXT("/Game/Characters/Mannequins/Animations/ABP_Manny.ABP_Manny");

UAnimBlueprint* LoadLyraMannequinAnimBPForBlendSpace()
{
    return LoadObject<UAnimBlueprint>(nullptr, LyraMannequinAnimBPPath_BlendSpace);
}

// Mirrors CreateFreshAnimBlueprint in TestAnimGraphHandlers.cpp.
UAnimBlueprint* CreateFreshAnimBlueprintForBlendSpace(const FString& PackagePath, USkeleton* Skeleton)
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

// Walk a BlendSpace-bearing AnimBP and count its core structural pieces. The
// helper is shared by source and target asserts so the round-trip comparison
// is symmetric.
struct FBlendSpaceShape
{
    int32 BlendSpaceCount = 0;
    int32 SampleGraphCount = 0;
    int32 SequencePlayerCount = 0;
    int32 SampleResultCount = 0;
};

FBlendSpaceShape CollectBlendSpaceShape(UAnimBlueprint* AnimBP)
{
    FBlendSpaceShape Shape;
    if (!AnimBP)
    {
        return Shape;
    }
    UEdGraph* MainAnimGraph = AnimGraphConstructionUtils::GetAnimGraphFromBlueprint(AnimBP);
    if (!MainAnimGraph)
    {
        return Shape;
    }
    for (UEdGraphNode* Node : MainAnimGraph->Nodes)
    {
        UAnimGraphNode_BlendSpaceGraphBase* BS = Cast<UAnimGraphNode_BlendSpaceGraphBase>(Node);
        if (!BS)
        {
            continue;
        }
        ++Shape.BlendSpaceCount;
        UBlendSpaceGraph* OwningGraph = BS->GetBlendSpaceGraph();
        if (!OwningGraph)
        {
            continue;
        }
        for (UEdGraph* SubGraph : OwningGraph->SubGraphs)
        {
            UAnimationBlendSpaceSampleGraph* SampleGraph = Cast<UAnimationBlendSpaceSampleGraph>(SubGraph);
            if (!SampleGraph)
            {
                continue;
            }
            ++Shape.SampleGraphCount;
            for (UEdGraphNode* InnerNode : SampleGraph->Nodes)
            {
                if (Cast<UAnimGraphNode_SequencePlayer>(InnerNode))
                {
                    ++Shape.SequencePlayerCount;
                }
                else if (Cast<UAnimGraphNode_BlendSpaceSampleResult>(InnerNode))
                {
                    ++Shape.SampleResultCount;
                }
            }
        }
    }
    return Shape;
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAGIRBlendSpaceRoundTripTest,
    "PinWright.AGIR.BlendSpace.RoundTrip",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAGIRBlendSpaceRoundTripTest::RunTest(const FString& Parameters)
{
    PINWRIGHT_SKIP_IF_FIXTURE_MISSING(LyraMannequinAnimBPPath_BlendSpace);
    UAnimBlueprint* Fixture = LoadLyraMannequinAnimBPForBlendSpace();
    if (!TestNotNull(TEXT("Mannequin AnimBP fixture loaded"), Fixture)) return false;
    if (!TestNotNull(TEXT("fixture has TargetSkeleton"), Fixture->TargetSkeleton.Get())) return false;
    USkeleton* Skeleton = Fixture->TargetSkeleton;

    const FString Guid = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString SourcePath = FString::Printf(
        TEXT("/Game/PinWrightTests/ABP_AGIR_BS_Src_%s"), *Guid);
    const FString TargetPath = FString::Printf(
        TEXT("/Game/PinWrightTests/ABP_AGIR_BS_%s"), *Guid);

    ON_SCOPE_EXIT
    {
        CleanupTestAsset(SourcePath);
        CleanupTestAsset(TargetPath);
    };

    UAnimBlueprint* SourceBP = CreateFreshAnimBlueprintForBlendSpace(SourcePath, Skeleton);
    TestNotNull(TEXT("source AnimBP created"), SourceBP);
    if (!SourceBP)
    {
        return false;
    }

    // Stage 1: compile a hand-written AGIR text into the source AnimBP. This is
    // the only reliable way to author a UAnimGraphNode_BlendSpaceGraph fixture
    // without an external UBlendSpace asset (engine `SetupFromAsset` requires
    // one); the compile path itself populates the dummy parent graph + sample
    // sub-graphs. Wave 2's compile coverage lets us reuse it as fixture builder.
    const FString SeedAGIRText = TEXT(
        "entry anim_graph \"AnimGraph\" {\n"
        "    blend_space \"TestBlendSpace\" {\n"
        "        sample_graph \"Sample0\" {\n"
        "            %n0 = call `/Script/AnimGraph.AnimGraphNode_SequencePlayer`\n"
        "            output %n0\n"
        "        }\n"
        "    }\n"
        "}\n");

    FAGIRCompileOptions SeedOptions;
    SeedOptions.Context = SourceBP->GetPathName();
    SeedOptions.Mode = EAGIRCompileMode::Replace;
    SeedOptions.bRunLayout = false;
    SeedOptions.bSave = false;
    FAGIRCompileResult SeedResult = FAGIRCompiler::Compile(SeedAGIRText, SeedOptions);
    TestTrue(TEXT("seed compile reports success"), SeedResult.bSuccess);
    if (!SeedResult.bSuccess)
    {
        AddError(FString::Printf(TEXT("BlendSpaceRoundTrip seed compile: %s: %s"),
            *SeedResult.ErrorCode, *SeedResult.ErrorMessage));
        return false;
    }

    const FBlendSpaceShape SourceShape = CollectBlendSpaceShape(SourceBP);
    TestEqual(TEXT("source has one blend_space"), SourceShape.BlendSpaceCount, 1);
    TestEqual(TEXT("source has one sample_graph"), SourceShape.SampleGraphCount, 1);
    TestEqual(TEXT("source sample has one SequencePlayer"), SourceShape.SequencePlayerCount, 1);
    TestEqual(TEXT("source sample has one SampleResult"), SourceShape.SampleResultCount, 1);

    // Stage 2: decompile the source -> AGIR text. Wave 3 emits the block-form
    // `blend_space "Name" { sample_graph ... }` shape with embedded `output %n`
    // for the sample result wiring.
    FAGIRDecompileResult DecompileResult = FAGIRDecompiler(SourceBP).Decompile();
    TestTrue(TEXT("decompile reports success"), DecompileResult.bSuccess);
    if (!DecompileResult.bSuccess)
    {
        const FString FirstWarning = DecompileResult.Warnings.Num() > 0
            ? DecompileResult.Warnings[0] : FString();
        AddError(FString::Printf(TEXT("BlendSpaceRoundTrip decompile: %s"), *FirstWarning));
        return false;
    }
    TestTrue(TEXT("decompile text contains 'sample_graph'"),
        DecompileResult.AGIRText.Contains(TEXT("sample_graph")));

    // Stage 3: compile decompiled text into a fresh target AnimBP.
    UAnimBlueprint* TargetBP = CreateFreshAnimBlueprintForBlendSpace(TargetPath, Skeleton);
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
    TestTrue(TEXT("round-trip compile reports success"), CompileResult.bSuccess);
    if (!CompileResult.bSuccess)
    {
        AddError(FString::Printf(TEXT("BlendSpaceRoundTrip target compile: %s: %s"),
            *CompileResult.ErrorCode, *CompileResult.ErrorMessage));
        return false;
    }

    // Counterfactual: reverting EmitBlendSpaceGraph's block-opener extension
    // makes the round-tripped sample graph contain zero nodes; the structural
    // counts below would mismatch.
    const FBlendSpaceShape TargetShape = CollectBlendSpaceShape(TargetBP);
    TestEqual(TEXT("target blend_space count round-trips"),
        TargetShape.BlendSpaceCount, SourceShape.BlendSpaceCount);
    TestEqual(TEXT("target sample_graph count round-trips"),
        TargetShape.SampleGraphCount, SourceShape.SampleGraphCount);
    TestEqual(TEXT("target SequencePlayer count round-trips"),
        TargetShape.SequencePlayerCount, SourceShape.SequencePlayerCount);
    TestEqual(TEXT("target SampleResult count round-trips"),
        TargetShape.SampleResultCount, SourceShape.SampleResultCount);

    return true;
}
