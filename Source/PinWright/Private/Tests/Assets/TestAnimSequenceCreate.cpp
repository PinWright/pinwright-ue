// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"

#include "Compat/EngineVersionCompat.h"
#include "Handlers/Animation/AnimSequenceCreate.h"
#include "Handlers/ErrorCodes.h"
#include "Tests/Assets/AnimAuthoringTestFixtures.h"
#include "Tests/TestSkipReporting.h"

#include "Animation/AnimData/IAnimationDataController.h"
#include "Animation/AnimData/IAnimationDataModel.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAnimSequenceCreateKeyCountMismatchTest,
    "PinWright.Assets.AnimSequenceCreate.KeyCountMismatchIsRefused",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAnimSequenceCreateKeyCountMismatchTest::RunTest(const FString& Parameters)
{
#if UE_VERSION_OLDER_THAN(5, 5, 0)
    // Closing a controller bracket on a hand-authored transient AnimSequence can trigger the
    // engine's anim-compression DDC hard assert on UE 5.3/5.4.  Keep this guard on the direct
    // seam test as well as the existing dump-builder test; it is an engine limitation, not a
    // reason to weaken the production count check.
    PinWrightTestSkip::SkipAssertions(*this, TEXT("engine-version-unsupported"),
        TEXT("Skipping AnimSequenceCreate.KeyCountMismatchIsRefused on UE 5.3/5.4: "
             "engine anim compression asserts while closing a transient authoring bracket."));
    return true;
#else
    FString ObjectPath;
    UAnimSequence* Sequence = AnimAuthoringTestFixtures::NewTransientAnimSequence(ObjectPath);
    AnimAuthoringTestFixtures::FScopedAnimAssetRoot SequenceRoot(Sequence);
    TestNotNull(TEXT("Transient AnimSequence created"), Sequence);
    if (!Sequence)
    {
        return false;
    }
    const FName BoneName(TEXT("pelvis"));
    USkeleton* Skeleton = AnimAuthoringTestFixtures::NewTransientSkeletonWithBones({BoneName});
    AnimAuthoringTestFixtures::FScopedAnimAssetRoot SkeletonRoot(Skeleton);
    TestNotNull(TEXT("Transient skeleton created"), Skeleton);
    if (!Skeleton)
    {
        return false;
    }
    Sequence->SetSkeleton(Skeleton);

    {
        IAnimationDataController& Controller = Sequence->GetController();
#if WITH_EDITOR
        IAnimationDataController::FScopedBracket Bracket(
            Controller, FText::FromString(TEXT("AnimSequenceCreate mismatch test")), false);
#endif
        Controller.InitializeModel();
        Controller.SetNumberOfFrames(FFrameNumber(48), false);
    }

    const IAnimationDataModel* BeforeModel = Sequence->GetDataModel();
    TestNotNull(TEXT("Animation data model exists before the refused write"), BeforeModel);
    if (!BeforeModel)
    {
        return false;
    }
    const int32 BeforeTrackCount = BeforeModel->GetNumBoneTracks();
    const int32 BeforeKeyCount = BeforeModel->GetNumberOfKeys();

    FPwBoneTrackSpec ShortTrack;
    ShortTrack.BoneName = BoneName;
    ShortTrack.Pos.Init(FVector3f::ZeroVector, 8);
    ShortTrack.Rot.Init(FQuat4f(0.0f, 0.0f, 0.0f, 1.0f), 8);
    ShortTrack.Scale.Init(FVector3f::OneVector, 8);

    const TArrayView<const FPwBoneTrackSpec> Tracks(&ShortTrack, 1);
    const FAnimSequenceWriteResult Result = WriteBoneTracks(Sequence, Tracks, false);
    TestFalse(TEXT("An 8-key track is refused on a 48-frame sequence"), Result.bSuccess);
    TestEqual(TEXT("Short-track refusal is typed as ANIMATION_INVALID"),
        Result.ErrorCode, FString(ErrorCodes::ERR_ANIMATION_INVALID));

    const IAnimationDataModel* AfterModel = Sequence->GetDataModel();
    TestNotNull(TEXT("Animation data model remains available after refusal"), AfterModel);
    if (AfterModel)
    {
        TestEqual(TEXT("Refused write does not add a bone track"),
            AfterModel->GetNumBoneTracks(), BeforeTrackCount);
        TestEqual(TEXT("Refused write does not change model key count"),
            AfterModel->GetNumberOfKeys(), BeforeKeyCount);
    }
    return true;
#endif
}
