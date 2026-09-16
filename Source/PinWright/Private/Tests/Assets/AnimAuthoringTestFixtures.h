// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Misc/Guid.h"

#include "Animation/AnimSequence.h"
#include "Animation/Skeleton.h"
#include "ReferenceSkeleton.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

// These fixtures are header-only because several animation test translation units are merged
// into one Unity compilation unit.  Inline functions in a named namespace keep the helpers
// reusable without creating external-symbol or anonymous-namespace collisions.
namespace AnimAuthoringTestFixtures
{
    class FScopedAnimAssetRoot
    {
    public:
        explicit FScopedAnimAssetRoot(UObject* InAsset)
            : Asset(InAsset)
        {
        }

        ~FScopedAnimAssetRoot()
        {
            Reset();
        }

        FScopedAnimAssetRoot(const FScopedAnimAssetRoot&) = delete;
        FScopedAnimAssetRoot& operator=(const FScopedAnimAssetRoot&) = delete;

        FScopedAnimAssetRoot(FScopedAnimAssetRoot&& Other) noexcept
            : Asset(Other.Asset)
        {
            Other.Asset = nullptr;
        }

        FScopedAnimAssetRoot& operator=(FScopedAnimAssetRoot&& Other) noexcept
        {
            if (this != &Other)
            {
                Reset();
                Asset = Other.Asset;
                Other.Asset = nullptr;
            }
            return *this;
        }

        void Reset()
        {
            if (Asset && Asset->IsRooted())
            {
                Asset->RemoveFromRoot();
            }
            Asset = nullptr;
        }

    private:
        UObject* Asset = nullptr;
    };

    inline FString MakeUniqueAnimSeqTestAssetName(const TCHAR* Prefix)
    {
        return FString::Printf(TEXT("%s_%s"), Prefix,
            *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    }

    // These factories deliberately transfer root ownership to the caller. Construct a
    // FScopedAnimAssetRoot from the returned pointer as the next statement so every return path
    // releases the root.
    inline UAnimSequence* NewTransientAnimSequence(FString& OutObjectPath)
    {
        const FString AssetName = MakeUniqueAnimSeqTestAssetName(TEXT("AS_AnimSequenceDump"));
        const FString PackageName = FString::Printf(TEXT("/Game/PinWrightTests/%s"), *AssetName);
        UPackage* Package = CreatePackage(*PackageName);
        UAnimSequence* Sequence = NewObject<UAnimSequence>(
            Package, *AssetName, RF_Public | RF_Standalone | RF_Transient);
        if (!Sequence)
        {
            return nullptr;
        }

        FAnimNotifyEvent Event;
        Event.NotifyName = FName(TEXT("TestNotify"));
        Event.SetTime(0.25f);
        Sequence->Notifies.Add(Event);

        Sequence->AddToRoot();
        OutObjectPath = FString::Printf(TEXT("%s.%s"), *PackageName, *AssetName);
        return Sequence;
    }

    // Build a transient skeleton whose reference skeleton contains the given bones (root first,
    // the rest parented to root).  RefBonePoses is optional so existing dump tests keep their
    // identity-pose behavior while animation compiler tests can deliberately exercise a
    // non-identity reference pose.
    inline USkeleton* NewTransientSkeletonWithBones(
        const TArray<FName>& BoneNames,
        const TArray<FTransform>& RefBonePoses = TArray<FTransform>())
    {
        const FString AssetName = MakeUniqueAnimSeqTestAssetName(TEXT("SK_AnimSequenceDump"));
        const FString PackageName = FString::Printf(TEXT("/Game/PinWrightTests/%s"), *AssetName);
        UPackage* Package = CreatePackage(*PackageName);
        USkeleton* Skeleton = NewObject<USkeleton>(
            Package, *AssetName, RF_Public | RF_Standalone | RF_Transient);
        if (!Skeleton)
        {
            return nullptr;
        }

        {
            FReferenceSkeletonModifier Modifier(Skeleton);
            for (int32 Index = 0; Index < BoneNames.Num(); ++Index)
            {
                FMeshBoneInfo BoneInfo;
                BoneInfo.Name = BoneNames[Index];
                BoneInfo.ExportName = BoneNames[Index].ToString();
                BoneInfo.ParentIndex = (Index == 0) ? INDEX_NONE : 0;
                const FTransform RefPose = RefBonePoses.IsValidIndex(Index)
                    ? RefBonePoses[Index]
                    : FTransform::Identity;
                Modifier.Add(BoneInfo, RefPose, /*bAllowMultipleRoots=*/Index == 0);
            }
        }

        Skeleton->AddToRoot();
        return Skeleton;
    }
}
