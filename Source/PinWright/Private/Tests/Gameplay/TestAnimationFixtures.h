// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

// Shared animation fixtures for tests that need a resolvable animation asset.
// These are deliberately real in-memory assets: animation handlers use the asset
// registry and the editor asset subsystem, so RF_Transient plus AddToRoot is not
// an adequate fixture.

#include "AssetRegistry/AssetRegistryModule.h"
#include "Animation/AnimData/IAnimationDataController.h"
#include "Animation/AnimSequence.h"
#include "Animation/Skeleton.h"
#include "Misc/EngineVersionComparison.h"
#include "Misc/FrameNumber.h"
#include "Misc/FrameRate.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "ReferenceSkeleton.h"
#include "Tests/TestUtils.h"
#include "UObject/Package.h"

namespace PinWrightAnimationTestFixtures
{
    struct FRegisteredAnimationFixture
    {
        FRegisteredAnimationFixture() = default;

        ~FRegisteredAnimationFixture()
        {
            Reset();
        }

        FRegisteredAnimationFixture(const FRegisteredAnimationFixture&) = delete;
        FRegisteredAnimationFixture& operator=(const FRegisteredAnimationFixture&) = delete;

        // Creates a unique /Game package with a registered skeleton and sequence.
        // When FootLocations is supplied, the skeleton includes foot_l and the sequence
        // receives that exact track.
        bool Create(const TCHAR* Prefix, bool bAddFootBone = false,
            const FFrameRate& FrameRate = FFrameRate(30, 1),
            const TArray<FVector>* FootLocations = nullptr)
        {
            Reset();

            const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
            PackagePath = FString::Printf(
                TEXT("/Game/PinWrightTests/PWAnim_%s_%s"), Prefix, *Suffix);
            UPackage* Package = CreatePackage(*PackagePath);
            if (!Package)
            {
                return false;
            }

            const FString AssetName = FPackageName::GetLongPackageAssetName(PackagePath);
            Skeleton = NewObject<USkeleton>(
                Package, *(AssetName + TEXT("_Skeleton")), RF_Public | RF_Standalone);
            if (!Skeleton)
            {
                return false;
            }

            {
                FReferenceSkeletonModifier Modifier(Skeleton);
                Modifier.Add(
                    FMeshBoneInfo(FName(TEXT("root")), TEXT("root"), INDEX_NONE),
                    FTransform::Identity, /*bAllowMultipleRoots=*/true);
                if (bAddFootBone)
                {
                    Modifier.Add(
                        FMeshBoneInfo(FName(TEXT("foot_l")), TEXT("foot_l"), 0),
                        FTransform::Identity);
                }
            }

            Skeleton->AddToRoot();
            SkeletonObjectPath = Skeleton->GetPathName();
            FAssetRegistryModule::AssetCreated(Skeleton);
            bSkeletonRegistered = true;

            Sequence = NewObject<UAnimSequence>(
                Package, *AssetName, RF_Public | RF_Standalone);
            if (!Sequence)
            {
                return false;
            }
            Sequence->SetSkeleton(Skeleton);

            // 24000/1001 is used by the parity test. 800 frames is the smallest
            // count that resamples to an integral number of 30 fps frames, so
            // fixture creation does not raise the engine's fractional-frame ensure.
            int32 NumberOfFrames = 2;
            if (FrameRate.Numerator == 24000 && FrameRate.Denominator == 1001)
            {
                NumberOfFrames = 800;
            }
            if (FootLocations && FootLocations->Num() > 1)
            {
                NumberOfFrames = FootLocations->Num() - 1;
            }

            IAnimationDataController& Controller = Sequence->GetController();
            Controller.InitializeModel();
            Controller.SetFrameRate(FrameRate);
            Controller.SetNumberOfFrames(FFrameNumber(NumberOfFrames));

#if !UE_VERSION_OLDER_THAN(5, 5, 0)
            if (Skeleton->GetReferenceSkeleton().GetRawBoneNum() > 0)
            {
                const FReferenceSkeleton& RefSkeleton = Skeleton->GetReferenceSkeleton();
                const FName RootBoneName = RefSkeleton.GetBoneName(0);
                const FTransform& RootPose = RefSkeleton.GetRawRefBonePose()[0];
                const int32 NumKeys = NumberOfFrames + 1;
                TArray<FVector3f> PositionalKeys;
                TArray<FQuat4f> RotationalKeys;
                TArray<FVector3f> ScalingKeys;
                PositionalKeys.Init(FVector3f(RootPose.GetTranslation()), NumKeys);
                RotationalKeys.Init(FQuat4f(RootPose.GetRotation()), NumKeys);
                ScalingKeys.Init(FVector3f(RootPose.GetScale3D()), NumKeys);
                Controller.AddBoneCurve(RootBoneName);
                Controller.SetBoneTrackKeys(
                    RootBoneName, PositionalKeys, RotationalKeys, ScalingKeys);
            }
#endif

            if (FootLocations)
            {
                if (!bAddFootBone || FootLocations->Num() != NumberOfFrames + 1)
                {
                    return false;
                }
                TArray<FQuat> Rotations;
                TArray<FVector> Scales;
                Rotations.Init(FQuat::Identity, FootLocations->Num());
                Scales.Init(FVector::OneVector, FootLocations->Num());
                const FName FootBone(TEXT("foot_l"));
                Controller.AddBoneCurve(FootBone);
                Controller.SetBoneTrackKeys(
                    FootBone, *FootLocations, Rotations, Scales);
            }

            Controller.NotifyPopulated();
            Sequence->AddToRoot();
            SequenceObjectPath = Sequence->GetPathName();
            FAssetRegistryModule::AssetCreated(Sequence);
            bSequenceRegistered = true;
            return true;
        }

        void Reset()
        {
            if (Sequence)
            {
                const FString ObjectPath = SequenceObjectPath.IsEmpty()
                    ? Sequence->GetPathName()
                    : SequenceObjectPath;
                if (UPackage* Package = Sequence->GetOutermost())
                {
                    Package->SetDirtyFlag(false);
                }
                Sequence->RemoveFromRoot();
                if (bSequenceRegistered)
                {
                    CleanupTestAsset(ObjectPath);
                }
                Sequence = nullptr;
                bSequenceRegistered = false;
            }

            if (Skeleton)
            {
                const FString ObjectPath = SkeletonObjectPath.IsEmpty()
                    ? Skeleton->GetPathName()
                    : SkeletonObjectPath;
                if (UPackage* Package = Skeleton->GetOutermost())
                {
                    Package->SetDirtyFlag(false);
                }
                Skeleton->RemoveFromRoot();
                if (bSkeletonRegistered)
                {
                    CleanupTestAsset(ObjectPath);
                }
                Skeleton = nullptr;
                bSkeletonRegistered = false;
            }

            SequenceObjectPath.Empty();
            SkeletonObjectPath.Empty();
            PackagePath.Empty();
        }

        bool IsValid() const
        {
            return Sequence != nullptr && Skeleton != nullptr;
        }

        UAnimSequence* Sequence = nullptr;
        USkeleton* Skeleton = nullptr;
        FString PackagePath;
        FString SequenceObjectPath;
        FString SkeletonObjectPath;

    private:
        bool bSequenceRegistered = false;
        bool bSkeletonRegistered = false;
    };
}
