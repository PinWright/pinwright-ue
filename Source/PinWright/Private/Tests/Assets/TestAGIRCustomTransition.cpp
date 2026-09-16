// Copyright (c) 2026 Alexander Penkin. MIT License.

// Wave 3 chunk wave-3-decompile-bodies regression test.
//
// Round-trip: build a source AnimBP carrying a TLT_Custom transition with a
// non-trivial body (UAnimGraphNode_BlendListByBool wired to the result), then
// run FAGIRDecompiler.Decompile() to capture round-trip AGIR text including
// the `custom_transition_body { ... }` block, then compile that text into a
// fresh target AnimBP. Asserts the body content survives the full
// compile→decompile→compile loop.
#include "Misc/AutomationTest.h"


#include "AGIR/AGIRCompiler.h"
#include "AGIR/AGIRDecompiler.h"
#include "AnimGraphNode_BlendListByBool.h"
#include "AnimGraphNode_CustomTransitionResult.h"
#include "AnimGraphNode_StateMachine.h"
#include "AnimStateNode.h"
#include "AnimStateTransitionNode.h"
#include "Animation/AnimBlueprint.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimStateMachineTypes.h"
#include "AnimationCustomTransitionGraph.h"
#include "AnimationGraph.h"
#include "AnimationStateMachineGraph.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "EdGraph/EdGraph.h"
#include "Factories/AnimBlueprintFactory.h"
#include "Handlers/Animation/AnimGraphConstructionUtils.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Misc/Guid.h"
#include "Misc/ScopeExit.h"
#include "Tests/TestUtils.h"
#include "UObject/Package.h"

namespace
{
const TCHAR* LyraMannequinAnimBPPath_CustomTransition =
    TEXT("/Game/Characters/Mannequins/Animations/ABP_Manny.ABP_Manny");

UAnimBlueprint* LoadLyraMannequinAnimBPForCustomTransition()
{
    return LoadObject<UAnimBlueprint>(nullptr, LyraMannequinAnimBPPath_CustomTransition);
}

// Local mirror of CreateFreshAnimBlueprint (TestAnimGraphHandlers.cpp:105-130)
// so the test file stays self-contained.
UAnimBlueprint* CreateFreshAnimBlueprintForCustomTransition(const FString& PackagePath, USkeleton* Skeleton)
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

// Walk an anim graph for the first state machine that contains a transition
// node whose LogicType is TLT_Custom. Returns nullptr if absent.
UAnimStateTransitionNode* FindCustomTransition(UAnimBlueprint* AnimBP)
{
    if (!AnimBP)
    {
        return nullptr;
    }
    UEdGraph* MainGraph = AnimGraphConstructionUtils::GetAnimGraphFromBlueprint(AnimBP);
    if (!MainGraph)
    {
        return nullptr;
    }
    for (UEdGraphNode* Node : MainGraph->Nodes)
    {
        UAnimGraphNode_StateMachine* MachineNode = Cast<UAnimGraphNode_StateMachine>(Node);
        if (!MachineNode || !MachineNode->EditorStateMachineGraph)
        {
            continue;
        }
        for (UEdGraphNode* InnerNode : MachineNode->EditorStateMachineGraph->Nodes)
        {
            UAnimStateTransitionNode* Trans = Cast<UAnimStateTransitionNode>(InnerNode);
            if (Trans && Trans->LogicType.GetValue() == ETransitionLogicType::TLT_Custom)
            {
                return Trans;
            }
        }
    }
    return nullptr;
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAGIRCustomTransitionRoundTripTest,
    "PinWright.AGIR.CustomTransition.RoundTrip",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAGIRCustomTransitionRoundTripTest::RunTest(const FString& Parameters)
{
    PINWRIGHT_SKIP_IF_FIXTURE_MISSING(LyraMannequinAnimBPPath_CustomTransition);
    UAnimBlueprint* Fixture = LoadLyraMannequinAnimBPForCustomTransition();
    if (!TestNotNull(TEXT("Mannequin AnimBP fixture loaded"), Fixture)) return false;
    if (!TestNotNull(TEXT("fixture has TargetSkeleton"), Fixture->TargetSkeleton.Get())) return false;
    USkeleton* Skeleton = Fixture->TargetSkeleton;

    const FString Guid = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString SourcePath = FString::Printf(
        TEXT("/Game/PinWrightTests/ABP_AGIRCustomTransition_Src_%s"), *Guid);
    const FString TargetPath = FString::Printf(
        TEXT("/Game/PinWrightTests/ABP_AGIRCustomTransition_%s"), *Guid);

    ON_SCOPE_EXIT
    {
        CleanupTestAsset(SourcePath);
        CleanupTestAsset(TargetPath);
    };

    UAnimBlueprint* SourceBP = CreateFreshAnimBlueprintForCustomTransition(SourcePath, Skeleton);
    TestNotNull(TEXT("source AnimBP created"), SourceBP);
    if (!SourceBP)
    {
        return false;
    }

    // Stage 1: compile a hand-written AGIR text into the source AnimBP. The
    // body carries a UAnimGraphNode_BlendListByBool wired into the schema-
    // default UAnimGraphNode_CustomTransitionResult sink via `output %nN`.
    const FString SeedAGIRText = TEXT(
        "entry anim_graph \"AnimGraph\" {\n"
        "    state_machine \"Locomotion\" {\n"
        "        state \"Idle\" {\n"
        "        }\n"
        "        state \"Run\" {\n"
        "        }\n"
        "        transition Idle -> Run priority=0 rule=IdleToRun_Rule "
        "crossfade_duration=0.2 blend_mode=0 logic_type=2 {\n"
        "            custom_transition_body {\n"
        "                %n0 = call `/Script/AnimGraph.AnimGraphNode_BlendListByBool`\n"
        "                output %n0\n"
        "            }\n"
        "        }\n"
        "    }\n"
        "}\n");

    FAGIRCompileOptions SeedOptions;
    SeedOptions.Mode = EAGIRCompileMode::Replace;
    SeedOptions.Context = SourceBP->GetPathName();
    SeedOptions.bRunLayout = false;
    SeedOptions.bSave = false;
    FAGIRCompileResult SeedResult = FAGIRCompiler::Compile(SeedAGIRText, SeedOptions);
    TestTrue(TEXT("seed compile reports success"), SeedResult.bSuccess);
    if (!SeedResult.bSuccess)
    {
        AddError(FString::Printf(TEXT("CustomTransitionRoundTrip seed compile: %s: %s"),
            *SeedResult.ErrorCode, *SeedResult.ErrorMessage));
        return false;
    }

    UAnimStateTransitionNode* SourceTransNode = FindCustomTransition(SourceBP);
    TestNotNull(TEXT("source transition has TLT_Custom graph"), SourceTransNode);
    if (!SourceTransNode || !SourceTransNode->CustomTransitionGraph)
    {
        return false;
    }
    int32 SourceBlendListCount = 0;
    for (UEdGraphNode* InnerNode : SourceTransNode->CustomTransitionGraph->Nodes)
    {
        if (Cast<UAnimGraphNode_BlendListByBool>(InnerNode))
        {
            ++SourceBlendListCount;
        }
    }
    TestEqual(TEXT("source custom transition body has one BlendListByBool"),
        SourceBlendListCount, 1);

    // Stage 2: decompile -> AGIR text. Wave 3's EmitTransition extension emits
    // `transition From -> To attrs { custom_transition_body { ... } }` when
    // logic_type=TLT_Custom and the graph carries authored body nodes.
    FAGIRDecompileResult DecompileResult = FAGIRDecompiler(SourceBP).Decompile();
    TestTrue(TEXT("decompile reports success"), DecompileResult.bSuccess);
    if (!DecompileResult.bSuccess)
    {
        const FString FirstWarning = DecompileResult.Warnings.Num() > 0
            ? DecompileResult.Warnings[0] : FString();
        AddError(FString::Printf(TEXT("CustomTransitionRoundTrip decompile: %s"), *FirstWarning));
        return false;
    }
    TestTrue(TEXT("decompile text contains 'custom_transition_body'"),
        DecompileResult.AGIRText.Contains(TEXT("custom_transition_body")));

    // Stage 3: compile decompiled text into a fresh target AnimBP.
    UAnimBlueprint* TargetBP = CreateFreshAnimBlueprintForCustomTransition(TargetPath, Skeleton);
    TestNotNull(TEXT("target AnimBP created"), TargetBP);
    if (!TargetBP)
    {
        return false;
    }

    FAGIRCompileOptions Options;
    Options.Mode = EAGIRCompileMode::Replace;
    Options.Context = TargetBP->GetPathName();
    Options.bRunLayout = false;
    Options.bSave = false;
    FAGIRCompileResult Result = FAGIRCompiler::Compile(DecompileResult.AGIRText, Options);
    TestTrue(TEXT("round-trip compile reports success"), Result.bSuccess);
    if (!Result.bSuccess)
    {
        AddError(FString::Printf(TEXT("CustomTransitionRoundTrip target compile: %s: %s"),
            *Result.ErrorCode, *Result.ErrorMessage));
        return false;
    }

    // Counterfactual: reverting EmitTransition's block-form emit drops the
    // `custom_transition_body { ... }` block from the decompile output, so the
    // round-trip target's custom graph carries only the schema-default
    // CustomTransitionResult and the BlendListByBool count assertion fails.
    UAnimStateTransitionNode* TransNode = FindCustomTransition(TargetBP);
    TestNotNull(TEXT("target custom transition node materialised"), TransNode);
    if (!TransNode || !TransNode->CustomTransitionGraph)
    {
        return false;
    }

    TestEqual(TEXT("target transition logic type is TLT_Custom"),
        static_cast<int32>(TransNode->LogicType.GetValue()),
        static_cast<int32>(ETransitionLogicType::TLT_Custom));

    int32 TargetBlendListCount = 0;
    bool bTargetHasResultNode = false;
    for (UEdGraphNode* InnerNode : TransNode->CustomTransitionGraph->Nodes)
    {
        if (Cast<UAnimGraphNode_BlendListByBool>(InnerNode))
        {
            ++TargetBlendListCount;
        }
        else if (Cast<UAnimGraphNode_CustomTransitionResult>(InnerNode))
        {
            bTargetHasResultNode = true;
        }
    }
    TestEqual(TEXT("target custom transition body BlendListByBool count round-trips"),
        TargetBlendListCount, SourceBlendListCount);
    TestTrue(TEXT("target custom transition graph still has CustomTransitionResult"),
        bTargetHasResultNode);

    return true;
}
