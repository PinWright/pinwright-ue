// Copyright (c) 2026 Alexander Penkin. MIT License.

// Red test for B-agir-cached-pose-forward-ref-corruption.
//
// Reproduces the forward-reference cached-pose corruption: a use_cached_pose
// that consumes a cache BEFORE the matching save_cached_pose appears in the
// AGIR text (the normal locomotion emission order — the AnimGraph output line
// precedes the cache-body block). CompileBlockIntoGraph is a single pass
// sharing one CacheNameMap; when the use is compiled the save name is not yet
// in the map, so CompileUseCachedPoseInstruction takes the else branch and
// leaves UseNode->SaveCachedPoseNode null (only warning
// AGIR_CACHED_POSE_FORWARD_REF). No deferred second pass resolves it.
//
// The sibling PinWright.AGIR.CachedPose.RoundTrip already asserts the correct
// invariant — SaveCachedPoseNode resolves at AGIR compile time — but only for
// the backward-reference ordering (save emitted before use). This test asserts
// the SAME invariant for the forward-reference ordering, which the defect
// violates: the weak pointer is left null and the decompiled source= label
// drops to empty (AGIRTextEmitter derives it from SaveCachedPoseNode->CacheName).
#include "Misc/AutomationTest.h"


#include "AGIR/AGIRCompiler.h"
#include "AGIR/AGIRDecompiler.h"
#include "AnimGraphNode_SaveCachedPose.h"
#include "AnimGraphNode_UseCachedPose.h"
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
#include "UObject/Package.h"

namespace
{
const TCHAR* MannequinAnimBPPath_ForwardRef =
    TEXT("/Game/Characters/Mannequins/Animations/ABP_Manny.ABP_Manny");

// Mirrors CreateFreshAnimBlueprint in the sibling AGIR tests; redeclared
// locally to keep this test file self-contained.
UAnimBlueprint* CreateFreshAnimBlueprintForForwardRef(const FString& PackagePath, USkeleton* Skeleton)
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

template <typename TNode>
TNode* FindFirstOfClassForwardRef(UEdGraph* Graph)
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAGIRCachedPoseForwardRefTest,
    "PinWright.AGIR.CachedPose.ForwardRefResolvesLinkage",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAGIRCachedPoseForwardRefTest::RunTest(const FString& Parameters)
{
    PINWRIGHT_SKIP_IF_FIXTURE_MISSING(MannequinAnimBPPath_ForwardRef);

    UAnimBlueprint* Fixture = LoadObject<UAnimBlueprint>(nullptr, MannequinAnimBPPath_ForwardRef);
    if (!TestNotNull(TEXT("Mannequin AnimBP fixture loaded"), Fixture)) return false;
    if (!TestNotNull(TEXT("fixture has TargetSkeleton"), Fixture->TargetSkeleton.Get())) return false;
    USkeleton* Skeleton = Fixture->TargetSkeleton;

    const FString Guid = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString TargetPath = FString::Printf(
        TEXT("/Game/PinWrightTests/ABP_AGIR_CP_FwdRef_%s"), *Guid);

    ON_SCOPE_EXIT
    {
        CleanupTestAsset(TargetPath);
    };

    UAnimBlueprint* TargetBP = CreateFreshAnimBlueprintForForwardRef(TargetPath, Skeleton);
    if (!TestNotNull(TEXT("target AnimBP created"), TargetBP)) return false;

    // Forward-reference AGIR: the use_cached_pose (%n0) and the output that
    // consumes it are emitted BEFORE the save_cached_pose (%n2) that defines
    // the "Locomotion" cache — the canonical locomotion emission order. Pose
    // wires (output <- %n0, save.Pose <- %n1) resolve in Pass 2 regardless of
    // order; the save/use cache linkage is what the single-pass compile fails
    // to resolve on this ordering.
    const FString ForwardRefAGIRText = TEXT(
        "entry anim_graph \"AnimGraph\" {\n"
        "    %n0 = use_cached_pose source=Locomotion\n"
        "    output %n0\n"
        "    %n1 = call `/Script/AnimGraph.AnimGraphNode_SequencePlayer`\n"
        "    %n2 = save_cached_pose name=Locomotion Pose=%n1\n"
        "}\n");

    FAGIRCompileOptions Options;
    Options.Context = TargetBP->GetPathName();
    Options.Mode = EAGIRCompileMode::Replace;
    Options.bRunLayout = false;
    Options.bSave = false;

    FAGIRCompileResult CompileResult = FAGIRCompiler::Compile(ForwardRefAGIRText, Options);
    // The forward reference is reported as a non-fatal warning, so the compile
    // itself succeeds today — that is exactly the trap the ticket describes
    // (tool reports success, linkage silently broken).
    if (!TestTrue(FString::Printf(TEXT("forward-ref compile succeeds (errorCode='%s', message='%s')"),
            *CompileResult.ErrorCode, *CompileResult.ErrorMessage), CompileResult.bSuccess))
    {
        return false;
    }

    UEdGraph* TargetAnimGraph = AnimGraphConstructionUtils::GetAnimGraphFromBlueprint(TargetBP);
    if (!TestNotNull(TEXT("target AnimGraph present"), TargetAnimGraph)) return false;

    UAnimGraphNode_SaveCachedPose* TargetSave =
        FindFirstOfClassForwardRef<UAnimGraphNode_SaveCachedPose>(TargetAnimGraph);
    UAnimGraphNode_UseCachedPose* TargetUse =
        FindFirstOfClassForwardRef<UAnimGraphNode_UseCachedPose>(TargetAnimGraph);
    if (!TestNotNull(TEXT("target has a SaveCachedPose node"), TargetSave)) return false;
    if (!TestNotNull(TEXT("target has a UseCachedPose node"), TargetUse)) return false;

    TestEqual(TEXT("SaveCachedPose CacheName is 'Locomotion'"),
        TargetSave->CacheName, FString(TEXT("Locomotion")));

    // PRIMARY differential assertion (fails pre-fix): with the use emitted
    // before the save, the single-pass compile leaves SaveCachedPoseNode null.
    // The correct behavior — the same invariant the backward-ref RoundTrip test
    // asserts — is that the use node links to its matching save node at AGIR
    // compile time. A two-pass / deferred cache resolution restores this.
    const bool bLinkResolved = TargetUse->SaveCachedPoseNode.IsValid();
    TestTrue(TEXT("UseCachedPose SaveCachedPoseNode resolves on a forward reference (use before save)"),
        bLinkResolved);
    if (bLinkResolved)
    {
        TestTrue(TEXT("UseCachedPose linked back to the correct Save node by CacheName"),
            TargetUse->SaveCachedPoseNode.Get() == TargetSave);
    }

    // Confirming symptom (ticket): the decompiler derives use_cached_pose's
    // source= label from SaveCachedPoseNode->CacheName, so a null link makes the
    // round-trip drop "Locomotion" to an empty source label. Correct behavior
    // round-trips the cache name.
    FAGIRDecompileResult DecompileResult = FAGIRDecompiler(TargetBP).Decompile();
    if (TestTrue(TEXT("decompile of forward-ref target succeeds"), DecompileResult.bSuccess))
    {
        TestTrue(TEXT("decompiled use_cached_pose carries source=Locomotion (not an empty label)"),
            DecompileResult.AGIRText.Contains(TEXT("use_cached_pose source=Locomotion")));
    }

    return true;
}
