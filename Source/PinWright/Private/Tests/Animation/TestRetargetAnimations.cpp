// Copyright (c) 2026 Alexander Penkin. MIT License.

// animation.retarget_animations: runs the IK Retargeter batch export on scratch fixtures under
// /Game/PinWrightTests. Two copies of /Engine/EngineMeshes/SkeletalCube (each on its own copied
// Skeleton) stand in for source and target characters; one IK Rig per mesh carries a single
// retarget chain on the cube's child bone, and the source clips rotate that bone 0 -> 90 degrees.
// The happy-path test asserts the rotation reaches the output, which a skeleton-swap copy
// (animation.setup_retargeting) cannot produce: the copy would keep the source tracks but the
// verb rebuilds the tracks from the retargeter, so this proves the export actually ran.

#include "Misc/AutomationTest.h"

#include "Animation/AnimData/IAnimationDataController.h"
#include "Animation/AnimData/IAnimationDataModel.h"
#include "Animation/AnimSequence.h"
#include "Animation/Skeleton.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EditorAssetLibrary.h"
#include "Engine/SkeletalMesh.h"
#include "Misc/EngineVersionComparison.h"
#include "Misc/FrameNumber.h"
#include "Misc/FrameRate.h"
#include "Misc/Guid.h"
#include "ReferenceSkeleton.h"
#include "Handlers/Animation/AnimationHandlerTestHooks.h"
#include "Tests/TestSkipReporting.h"
#include "Tests/TestUtils.h"
#include "UObject/Package.h"

#if __has_include("RetargetEditor/IKRetargetBatchOperation.h") \
    && __has_include("RetargetEditor/IKRetargeterController.h") \
    && __has_include("RigEditor/IKRigController.h")
#include "RetargetEditor/IKRetargeterController.h"
#include "Retargeter/IKRetargeter.h"
#include "Rig/IKRigDefinition.h"
#include "RigEditor/IKRigController.h"
#define PW_TEST_HAS_IK_BATCH_RETARGET 1
#else
#define PW_TEST_HAS_IK_BATCH_RETARGET 0
#endif

#if PW_TEST_HAS_IK_BATCH_RETARGET

namespace PwRetargetAnimationsTests
{
    const TCHAR* const Verb = TEXT("animation.retarget_animations");
    const TCHAR* const EngineMesh = TEXT("/Engine/EngineMeshes/SkeletalCube");
    const TCHAR* const EngineSkeleton = TEXT("/Engine/EngineMeshes/SkeletalCube_Skeleton");
    const FName ChainName(TEXT("Chain"));
    constexpr int32 ClipFrames = 10;

    TSet<FString> AssetsUnder(const FString& Folder, bool bRecursive)
    {
        IAssetRegistry& Registry =
            FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
        TArray<FAssetData> Found;
        Registry.GetAssetsByPath(FName(*Folder), Found, bRecursive);
        TSet<FString> Paths;
        for (const FAssetData& Data : Found)
        {
            Paths.Add(Data.PackageName.ToString());
        }
        return Paths;
    }

    FString ObjectPath(const FString& PackagePath)
    {
        return PackagePath + TEXT(".") + FPackageName::GetLongPackageAssetName(PackagePath);
    }

    struct FFixture
    {
        FString Folder;
        FString OutputFolder;
        TArray<FString> Packages;
        USkeletalMesh* SourceMesh = nullptr;
        USkeletalMesh* TargetMesh = nullptr;
        USkeleton* SourceSkeleton = nullptr;
        USkeleton* TargetSkeleton = nullptr;
        UIKRetargeter* Retargeter = nullptr;
        FString RetargeterPath;
        FName ChainBone;
        TArray<UAnimSequence*> Clips;

        ~FFixture()
        {
            TSet<FString> All(Packages);
            All.Append(AssetsUnder(Folder, /*bRecursive=*/true));
            for (const FString& Package : All)
            {
                CleanupTestAsset(Package);
            }
        }

        USkeletalMesh* CopyCube(const TCHAR* Tag, USkeleton*& OutSkeleton)
        {
            const FString SkeletonPath = Folder / FString::Printf(TEXT("SK_%s"), Tag);
            const FString MeshPath = Folder / FString::Printf(TEXT("SKM_%s"), Tag);
            OutSkeleton = Cast<USkeleton>(UEditorAssetLibrary::DuplicateAsset(EngineSkeleton, SkeletonPath));
            USkeletalMesh* Mesh = Cast<USkeletalMesh>(UEditorAssetLibrary::DuplicateAsset(EngineMesh, MeshPath));
            Packages.Add(SkeletonPath);
            Packages.Add(MeshPath);
            if (!OutSkeleton || !Mesh)
            {
                return nullptr;
            }
            Mesh->SetSkeleton(OutSkeleton);
            return Mesh;
        }

        UIKRigDefinition* MakeRig(FAutomationTestBase& Test, const TCHAR* Name, USkeletalMesh* Mesh)
        {
            TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
            Payload->SetStringField(TEXT("name"), Name);
            Payload->SetStringField(TEXT("path"), Folder);
            Payload->SetBoolField(TEXT("save"), false);
            FTestResponseCapture Capture;
            InvokeHandlerWithCapture(TEXT("animation.authoring.create_ik_rig"), Payload, Capture);
            FString RigPath;
            if (!Capture.bSuccess || !Capture.Result.IsValid()
                || !Capture.Result->TryGetStringField(TEXT("assetPath"), RigPath))
            {
                Test.AddError(FString::Printf(TEXT("create_ik_rig failed: %s %s"), *Capture.ErrorCode, *Capture.Message));
                return nullptr;
            }
            Packages.Add(FPackageName::ObjectPathToPackageName(RigPath));
            UIKRigDefinition* Rig = FindObject<UIKRigDefinition>(nullptr, *RigPath);
            UIKRigController* RigController = Rig ? UIKRigController::GetController(Rig) : nullptr;
            if (!RigController)
            {
                Test.AddError(TEXT("created IK Rig has no controller"));
                return nullptr;
            }
            const FReferenceSkeleton& Ref = Mesh->GetRefSkeleton();
            RigController->SetSkeletalMesh(Mesh);
            RigController->SetRetargetRoot(Ref.GetBoneName(0));
            if (RigController->AddRetargetChain(ChainName, ChainBone, ChainBone, NAME_None).IsNone())
            {
                Test.AddError(TEXT("AddRetargetChain refused the fixture chain"));
                return nullptr;
            }
            return Rig;
        }

        UAnimSequence* MakeClip(const FString& Name, USkeleton* Skeleton)
        {
            const FString PackagePath = Folder / Name;
            Packages.Add(PackagePath);
            UPackage* Package = CreatePackage(*PackagePath);
            UAnimSequence* Clip = NewObject<UAnimSequence>(Package, *Name, RF_Public | RF_Standalone);
            Clip->SetSkeleton(Skeleton);
            IAnimationDataController& Controller = Clip->GetController();
            Controller.InitializeModel();
            Controller.SetFrameRate(FFrameRate(30, 1));
            Controller.SetNumberOfFrames(FFrameNumber(ClipFrames));
            const FReferenceSkeleton& Ref = Skeleton->GetReferenceSkeleton();
            for (int32 BoneIndex = 0; BoneIndex < Ref.GetRawBoneNum(); ++BoneIndex)
            {
                const FName Bone = Ref.GetBoneName(BoneIndex);
                const FTransform& Pose = Ref.GetRawRefBonePose()[BoneIndex];
                TArray<FVector> Positions;
                TArray<FQuat> Rotations;
                TArray<FVector> Scales;
                for (int32 Key = 0; Key <= ClipFrames; ++Key)
                {
                    const double Degrees = Bone == ChainBone ? 90.0 * Key / ClipFrames : 0.0;
                    Positions.Add(Pose.GetTranslation());
                    Rotations.Add(Pose.GetRotation() * FQuat(FVector::UpVector, FMath::DegreesToRadians(Degrees)));
                    Scales.Add(Pose.GetScale3D());
                }
                Controller.AddBoneCurve(Bone);
                Controller.SetBoneTrackKeys(Bone, Positions, Rotations, Scales);
            }
            Controller.NotifyPopulated();
            FAssetRegistryModule::AssetCreated(Clip);
            return Clip;
        }

        // False (with a skip marker or an error already recorded) when the fixture cannot be built.
        bool Build(FAutomationTestBase& Test, int32 NumClips)
        {
            Folder = FString::Printf(TEXT("/Game/PinWrightTests/PWRetarget_%s"),
                *FGuid::NewGuid().ToString(EGuidFormats::Digits));
            OutputFolder = Folder / TEXT("Out");
            if (!UEditorAssetLibrary::DoesAssetExist(EngineMesh))
            {
                PinWrightTestSkip::SkipAssertions(Test, TEXT("engine-skeletal-cube-unavailable"),
                    TEXT("Skipped: /Engine/EngineMeshes/SkeletalCube unavailable."));
                return false;
            }
            SourceMesh = CopyCube(TEXT("Source"), SourceSkeleton);
            TargetMesh = CopyCube(TEXT("Target"), TargetSkeleton);
            if (!Test.TestTrue(TEXT("fixture meshes duplicated"), SourceMesh && TargetMesh))
            {
                return false;
            }
            const FReferenceSkeleton& Ref = SourceMesh->GetRefSkeleton();
            if (Ref.GetRawBoneNum() < 2)
            {
                PinWrightTestSkip::SkipAssertions(Test, TEXT("engine-skeletal-cube-single-bone"),
                    TEXT("Skipped: SkeletalCube has fewer than two bones; no child bone to animate."));
                return false;
            }
            ChainBone = Ref.GetBoneName(Ref.GetRawBoneNum() - 1);

            UIKRigDefinition* SourceRig = MakeRig(Test, TEXT("IKR_Source"), SourceMesh);
            UIKRigDefinition* TargetRig = MakeRig(Test, TEXT("IKR_Target"), TargetMesh);
            if (!SourceRig || !TargetRig)
            {
                return false;
            }

            TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
            Payload->SetStringField(TEXT("name"), TEXT("RTG_Fixture"));
            Payload->SetStringField(TEXT("path"), Folder);
            Payload->SetStringField(TEXT("sourceIKRigPath"), SourceRig->GetPathName());
            Payload->SetStringField(TEXT("targetIKRigPath"), TargetRig->GetPathName());
            Payload->SetBoolField(TEXT("save"), false);
            FTestResponseCapture Capture;
            InvokeHandlerWithCapture(TEXT("animation.authoring.create_ik_retargeter"), Payload, Capture);
            if (!Capture.bSuccess || !Capture.Result.IsValid()
                || !Capture.Result->TryGetStringField(TEXT("assetPath"), RetargeterPath))
            {
                Test.AddError(FString::Printf(TEXT("create_ik_retargeter failed: %s %s"), *Capture.ErrorCode, *Capture.Message));
                return false;
            }
            Packages.Add(FPackageName::ObjectPathToPackageName(RetargeterPath));
            Retargeter = FindObject<UIKRetargeter>(nullptr, *RetargeterPath);
            if (!Test.TestNotNull(TEXT("retargeter created"), Retargeter))
            {
                return false;
            }
#if UE_VERSION_OLDER_THAN(5, 6, 0)
            // No op stack before 5.6: the chain map lives on the asset.
            UIKRetargeterController::GetController(Retargeter)->AutoMapChains(EAutoMapChainType::Exact, true);
#endif

            for (int32 Index = 0; Index < NumClips; ++Index)
            {
                Clips.Add(MakeClip(FString::Printf(TEXT("Clip%d"), Index), SourceSkeleton));
            }
            return true;
        }

        TSharedPtr<FJsonObject> Payload(bool bSeedDefaultOps) const
        {
            TArray<TSharedPtr<FJsonValue>> Assets;
            for (const UAnimSequence* Clip : Clips)
            {
                Assets.Add(MakeShared<FJsonValueString>(Clip->GetPathName()));
            }
            TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
            Out->SetStringField(TEXT("retargeter"), RetargeterPath);
            Out->SetStringField(TEXT("sourceMesh"), SourceMesh->GetPathName());
            Out->SetStringField(TEXT("targetMesh"), TargetMesh->GetPathName());
            Out->SetArrayField(TEXT("assets"), Assets);
            Out->SetStringField(TEXT("outputPath"), OutputFolder);
            Out->SetStringField(TEXT("suffix"), TEXT("_RTG"));
            Out->SetBoolField(TEXT("seedDefaultOps"), bSeedDefaultOps);
            return Out;
        }

        FQuat ChainRotation(const UAnimSequence* Sequence, int32 Key) const
        {
            TArray<FTransform> Track;
            Sequence->GetDataModel()->GetBoneTrackTransforms(ChainBone, Track);
            return Track.IsValidIndex(Key) ? Track[Key].GetRotation() : FQuat::Identity;
        }
    };
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRetargetAnimationsRetargetsClipsTest,
    "PinWright.animation.retarget_animations.RetargetsClipsOntoTargetSkeleton",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FRetargetAnimationsRetargetsClipsTest::RunTest(const FString& Parameters)
{
    PwRetargetAnimationsTests::FFixture Fixture;
    if (!Fixture.Build(*this, 3))
    {
        return true;
    }

    FTestResponseCapture Capture;
    TestTrue(TEXT("handler registered"), InvokeHandlerWithCapture(PwRetargetAnimationsTests::Verb, Fixture.Payload(/*bSeedDefaultOps=*/true), Capture));
    if (!TestTrue(FString::Printf(TEXT("retarget succeeds (%s %s)"), *Capture.ErrorCode, *Capture.Message),
            Capture.bSuccess && Capture.Result.IsValid()))
    {
        return false;
    }

    const TArray<TSharedPtr<FJsonValue>>* Created = nullptr;
    TestTrue(TEXT("created[] present"), Capture.Result->TryGetArrayField(TEXT("created"), Created) && Created);
    TestEqual(TEXT("one verified output per clip"), Created ? Created->Num() : 0, 3);
    TestTrue(TEXT("mappingComplete"), Capture.Result->GetBoolField(TEXT("mappingComplete")));
    TestTrue(TEXT("outputs saved"), Capture.Result->GetBoolField(TEXT("saved")));

    for (const UAnimSequence* Clip : Fixture.Clips)
    {
        const FString OutputPath = PwRetargetAnimationsTests::ObjectPath(Fixture.OutputFolder / (Clip->GetName() + TEXT("_RTG")));
        const UAnimSequence* Output = FindObject<UAnimSequence>(nullptr, *OutputPath);
        if (!TestNotNull(*FString::Printf(TEXT("output %s exists"), *OutputPath), Output))
        {
            continue;
        }
        TestTrue(TEXT("output carries the target skeleton"), Output->GetSkeleton() == Fixture.TargetSkeleton);
        TestEqual(TEXT("output frame count matches the source"),
            Output->GetDataModel()->GetNumberOfFrames(), Clip->GetDataModel()->GetNumberOfFrames());
        TestTrue(TEXT("output has bone tracks"), Output->GetDataModel()->GetNumBoneTracks() > 0);
        TestTrue(TEXT("output .uasset is on disk"), FPackageName::DoesPackageExist(Output->GetOutermost()->GetName()));

        // The retargeted motion: the chain bone turns ~90 degrees across the clip, as in the source.
        const double SourceDegrees = FMath::RadiansToDegrees(
            Fixture.ChainRotation(Clip, 0).AngularDistance(Fixture.ChainRotation(Clip, PwRetargetAnimationsTests::ClipFrames)));
        const double OutputDegrees = FMath::RadiansToDegrees(
            Fixture.ChainRotation(Output, 0).AngularDistance(Fixture.ChainRotation(Output, PwRetargetAnimationsTests::ClipFrames)));
        TestTrue(FString::Printf(TEXT("source chain bone turns (%.1f deg)"), SourceDegrees), SourceDegrees > 80.0);
        TestTrue(FString::Printf(TEXT("retargeted chain bone turns like the source (%.1f vs %.1f deg)"),
            OutputDegrees, SourceDegrees), FMath::Abs(OutputDegrees - SourceDegrees) < 5.0);
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRetargetAnimationsUnmappedChainsTest,
    "PinWright.animation.retarget_animations.RefusesUnmappedTargetChains",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FRetargetAnimationsUnmappedChainsTest::RunTest(const FString& Parameters)
{
    PwRetargetAnimationsTests::FFixture Fixture;
    if (!Fixture.Build(*this, 1))
    {
        return true;
    }
    UIKRetargeterController* Controller = UIKRetargeterController::GetController(Fixture.Retargeter);
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 6, 0)
    Controller->AddDefaultOps();
#endif
    Controller->AutoMapChains(EAutoMapChainType::Clear, true);

    FTestResponseCapture Capture;
    InvokeHandlerWithCapture(PwRetargetAnimationsTests::Verb, Fixture.Payload(/*bSeedDefaultOps=*/false), Capture);
    TestFalse(TEXT("refused"), Capture.bSuccess);
    TestEqual(TEXT("error code"), Capture.ErrorCode, FString(TEXT("RETARGET_CHAINS_UNMAPPED")));
    TestTrue(TEXT("message names the unmapped chain"), Capture.Message.Contains(PwRetargetAnimationsTests::ChainName.ToString()));
    TestEqual(TEXT("nothing written"), PwRetargetAnimationsTests::AssetsUnder(Fixture.OutputFolder, true).Num(), 0);
    return true;
}

#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 6, 0)
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRetargetAnimationsNoOpsTest,
    "PinWright.animation.retarget_animations.RefusesRetargeterWithoutOps",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FRetargetAnimationsNoOpsTest::RunTest(const FString& Parameters)
{
    PwRetargetAnimationsTests::FFixture Fixture;
    if (!Fixture.Build(*this, 1))
    {
        return true;
    }
    UIKRetargeterController* Controller = UIKRetargeterController::GetController(Fixture.Retargeter);
    TestEqual(TEXT("create_ik_retargeter leaves an empty op stack"), Controller->GetNumRetargetOps(), 0);

    FTestResponseCapture Capture;
    InvokeHandlerWithCapture(PwRetargetAnimationsTests::Verb, Fixture.Payload(/*bSeedDefaultOps=*/false), Capture);
    TestFalse(TEXT("refused"), Capture.bSuccess);
    TestEqual(TEXT("error code"), Capture.ErrorCode, FString(TEXT("RETARGETER_NO_OPS")));
    TestEqual(TEXT("retargeter left untouched"), Controller->GetNumRetargetOps(), 0);
    TestEqual(TEXT("nothing written"), PwRetargetAnimationsTests::AssetsUnder(Fixture.OutputFolder, true).Num(), 0);
    return true;
}
#endif

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRetargetAnimationsSkeletonMismatchTest,
    "PinWright.animation.retarget_animations.RefusesClipOnAnotherSkeleton",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FRetargetAnimationsSkeletonMismatchTest::RunTest(const FString& Parameters)
{
    PwRetargetAnimationsTests::FFixture Fixture;
    if (!Fixture.Build(*this, 1))
    {
        return true;
    }
    Fixture.Clips.Add(Fixture.MakeClip(TEXT("ClipOnTarget"), Fixture.TargetSkeleton));

    FTestResponseCapture Capture;
    InvokeHandlerWithCapture(PwRetargetAnimationsTests::Verb, Fixture.Payload(/*bSeedDefaultOps=*/true), Capture);
    TestFalse(TEXT("refused"), Capture.bSuccess);
    TestEqual(TEXT("error code"), Capture.ErrorCode, FString(TEXT("SKELETON_MISMATCH")));
    TestTrue(TEXT("message names the clip"), Capture.Message.Contains(TEXT("ClipOnTarget")));
    TestEqual(TEXT("nothing written"), PwRetargetAnimationsTests::AssetsUnder(Fixture.OutputFolder, true).Num(), 0);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRetargetAnimationsPartialResultTest,
    "PinWright.animation.retarget_animations.DeletesEveryOutputOnPartialResult",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FRetargetAnimationsPartialResultTest::RunTest(const FString& Parameters)
{
    PwRetargetAnimationsTests::FFixture Fixture;
    if (!Fixture.Build(*this, 3))
    {
        return true;
    }

    FTestResponseCapture Capture;
    {
        AnimationHandlerTestHooks::FScopedRetargetVerifyFailure FailSecond(1);
        InvokeHandlerWithCapture(PwRetargetAnimationsTests::Verb, Fixture.Payload(/*bSeedDefaultOps=*/true), Capture);
    }
    TestFalse(TEXT("refused"), Capture.bSuccess);
    TestEqual(TEXT("error code"), Capture.ErrorCode, FString(TEXT("RETARGET_INCOMPLETE")));
    const TArray<TSharedPtr<FJsonValue>>* Removed = nullptr;
    const TArray<TSharedPtr<FJsonValue>>* Remaining = nullptr;
    if (TestTrue(TEXT("error payload present"), Capture.Result.IsValid()))
    {
        Capture.Result->TryGetArrayField(TEXT("removedOutputs"), Removed);
        Capture.Result->TryGetArrayField(TEXT("outputsRemaining"), Remaining);
    }
    TestEqual(TEXT("all three outputs reported removed"), Removed ? Removed->Num() : -1, 3);
    TestEqual(TEXT("none reported remaining"), Remaining ? Remaining->Num() : -1, 0);
    TestEqual(TEXT("registry holds no output"), PwRetargetAnimationsTests::AssetsUnder(Fixture.OutputFolder, true).Num(), 0);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRetargetAnimationsBadInputsTest,
    "PinWright.animation.retarget_animations.RefusesBadInputsBeforeLoading",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FRetargetAnimationsBadInputsTest::RunTest(const FString& Parameters)
{
    const FString Absent = FString::Printf(TEXT("/Game/PinWrightTests/PWRetargetAbsent_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    auto Make = [&Absent]()
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("retargeter"), Absent / TEXT("RTG"));
        Payload->SetStringField(TEXT("sourceMesh"), Absent / TEXT("Source"));
        Payload->SetStringField(TEXT("targetMesh"), Absent / TEXT("Target"));
        TArray<TSharedPtr<FJsonValue>> Assets;
        Assets.Add(MakeShared<FJsonValueString>(Absent / TEXT("Clip")));
        Payload->SetArrayField(TEXT("assets"), Assets);
        Payload->SetStringField(TEXT("outputPath"), Absent / TEXT("Out"));
        return Payload;
    };
    auto Expect = [this](const TCHAR* What, const TSharedPtr<FJsonObject>& Payload, const TCHAR* Code)
    {
        FTestResponseCapture Capture;
        InvokeHandlerWithCapture(PwRetargetAnimationsTests::Verb, Payload, Capture);
        TestFalse(FString::Printf(TEXT("%s: refused"), What), Capture.bSuccess);
        TestEqual(FString::Printf(TEXT("%s: error code"), What), Capture.ErrorCode, FString(Code));
    };

    TSharedPtr<FJsonObject> SameMesh = Make();
    SameMesh->SetStringField(TEXT("targetMesh"), Absent / TEXT("Source"));
    Expect(TEXT("same source and target mesh"), SameMesh, TEXT("INVALID_ARGUMENT"));

    TSharedPtr<FJsonObject> NoAssets = Make();
    NoAssets->SetArrayField(TEXT("assets"), TArray<TSharedPtr<FJsonValue>>());
    Expect(TEXT("empty assets"), NoAssets, TEXT("INVALID_ARGUMENT"));

    TSharedPtr<FJsonObject> ObjectOutput = Make();
    ObjectOutput->SetStringField(TEXT("outputPath"), Absent / TEXT("Out.Out"));
    Expect(TEXT("object path as outputPath"), ObjectOutput, TEXT("INVALID_PATH"));

    Expect(TEXT("missing retargeter and meshes"), Make(), TEXT("ASSET_NOT_FOUND"));
    TestFalse(TEXT("no output folder created"), UEditorAssetLibrary::DoesDirectoryExist(Absent / TEXT("Out")));
    return true;
}

#endif // PW_TEST_HAS_IK_BATCH_RETARGET
