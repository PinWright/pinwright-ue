// Copyright (c) 2026 Alexander Penkin. MIT License.

// Wave 1 chunk wave-1-classifier-heuristic regression test.
//
// `FAGIRDecompiler::ClassifyGraph` distinguishes anim function graphs from the
// main AnimGraph by name (non-"AnimGraph") + presence of a
// `UAnimGraphNode_LinkedInputPose`. The test seeds a synthetic AnimBP with
// both a default main AnimGraph and a hand-built function graph, decompiles,
// and asserts the per-graph header reflects the classification.
#include "Misc/AutomationTest.h"


#include "AGIR/AGIRDecompiler.h"
#include "AnimGraphNode_LinkedInputPose.h"
#include "AnimationGraph.h"
#include "AnimationGraphSchema.h"
#include "Animation/AnimBlueprint.h"
#include "Animation/AnimInstance.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
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
const TCHAR* LyraMannequinAnimBPPath_Classifier =
    TEXT("/Game/Characters/Mannequins/Animations/ABP_Manny.ABP_Manny");

UAnimBlueprint* LoadLyraMannequinAnimBPForClassifier()
{
    return LoadObject<UAnimBlueprint>(nullptr, LyraMannequinAnimBPPath_Classifier);
}

// Mirrors CreateFreshAnimBlueprint in TestAnimGraphHandlers.cpp; redeclared
// locally to keep this test file self-contained.
UAnimBlueprint* CreateFreshAnimBlueprintForClassifier(const FString& PackagePath, USkeleton* Skeleton)
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
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAGIRClassifierAnimFunctionTest,
    "PinWright.AGIR.Classifier.AnimFunction",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAGIRClassifierAnimFunctionTest::RunTest(const FString& Parameters)
{
    PINWRIGHT_SKIP_IF_FIXTURE_MISSING(LyraMannequinAnimBPPath_Classifier);
    UAnimBlueprint* Fixture = LoadLyraMannequinAnimBPForClassifier();
    if (!TestNotNull(TEXT("Mannequin AnimBP fixture loaded"), Fixture)) return false;
    if (!TestNotNull(TEXT("fixture has TargetSkeleton"), Fixture->TargetSkeleton.Get())) return false;
    USkeleton* Skeleton = Fixture->TargetSkeleton;

    const FString Guid = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString SourcePath = FString::Printf(
        TEXT("/Game/PinWrightTests/ABP_AGIRClassifier_%s"), *Guid);

    ON_SCOPE_EXIT
    {
        CleanupTestAsset(SourcePath);
    };

    UAnimBlueprint* SourceBP = CreateFreshAnimBlueprintForClassifier(SourcePath, Skeleton);
    if (!SourceBP)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-unavailable"),
            TEXT("ClassifierAnimFunction: could not create source AnimBlueprint - skipped."));
        return true;
    }

    // Seed a function graph alongside the default main AnimGraph. Naming the
    // graph anything other than `UEdGraphSchema_K2::GN_AnimGraph` plus adding
    // a LinkedInputPose triggers the AnimFunction branch in ClassifyGraph.
    UEdGraph* FuncGraph = FBlueprintEditorUtils::CreateNewGraph(
        SourceBP, FName(TEXT("MyAnimFunc")),
        UAnimationGraph::StaticClass(), UAnimationGraphSchema::StaticClass());
    TestNotNull(TEXT("function graph created"), FuncGraph);
    if (!FuncGraph)
    {
        return false;
    }

    SourceBP->FunctionGraphs.Add(FuncGraph);

    // Drop a single LinkedInputPose into the function body so the classifier
    // heuristic finds it. Position is arbitrary — the test only inspects the
    // decompile entry-kind header.
    FGraphNodeCreator<UAnimGraphNode_LinkedInputPose> NodeCreator(*FuncGraph);
    UAnimGraphNode_LinkedInputPose* LinkedInputNode = NodeCreator.CreateNode(/*bSelectNewNode=*/false);
    TestNotNull(TEXT("LinkedInputPose node created"), LinkedInputNode);
    if (!LinkedInputNode)
    {
        return false;
    }
    LinkedInputNode->Node.Name = TEXT("InPose");
    LinkedInputNode->InputPoseIndex = 0;
    NodeCreator.Finalize();

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(SourceBP);

    // Sanity: the default AnimGraph must still be present and named "AnimGraph".
    UEdGraph* MainAnimGraph = AnimGraphConstructionUtils::GetAnimGraphFromBlueprint(SourceBP);
    TestNotNull(TEXT("main AnimGraph present"), MainAnimGraph);
    if (MainAnimGraph)
    {
        TestEqual(TEXT("main AnimGraph keeps canonical name"),
            MainAnimGraph->GetFName(), UEdGraphSchema_K2::GN_AnimGraph);
    }

    FAGIRDecompileResult DecompileResult = FAGIRDecompiler(SourceBP).Decompile();
    TestTrue(TEXT("decompile reports success"), DecompileResult.bSuccess);
    if (!DecompileResult.bSuccess || DecompileResult.AGIRText.IsEmpty())
    {
        AddError(FString::Printf(TEXT("decompile produced no text (warnings: %d)"),
            DecompileResult.Warnings.Num()));
        return false;
    }

    // Decompiler emits a per-graph header line `# ==== Graph: <name> (<kind>) ====`.
    // Counterfactual: reverting ClassifyGraph to its pre-chunk form makes both
    // headers carry `(anim_graph)` and the `(anim_function)` assertion fails.
    const bool bHasFuncHeader = DecompileResult.AGIRText.Contains(
        TEXT("# ==== Graph: MyAnimFunc (anim_function) ===="));
    TestTrue(TEXT("MyAnimFunc graph classified as anim_function"), bHasFuncHeader);

    const bool bHasMainHeader = DecompileResult.AGIRText.Contains(
        TEXT("# ==== Graph: AnimGraph (anim_graph) ===="));
    TestTrue(TEXT("main AnimGraph classified as anim_graph"), bHasMainHeader);

    return true;
}
