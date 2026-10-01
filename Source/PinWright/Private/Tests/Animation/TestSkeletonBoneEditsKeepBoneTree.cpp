// Copyright (c) 2026 Alexander Penkin. MIT License.

// skeleton.remove_bone / skeleton.set_bone_parent and USkeleton::BoneTree.
//
// BoneTree holds each bone's translation retargeting mode and is read by raw bone index.
// FReferenceSkeletonModifier::Remove shifts every later bone down one slot and SetParent re-sorts
// parents before children, but neither touches BoneTree, so the modes used to stay on the old
// slots: after the edit they belonged to different bones and a stale entry trailed the array,
// all under a success response. Each fixture bone carries a distinct mode so any shift shows.
#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/Guid.h"
#include "Misc/ScopeExit.h"
#include "Tests/TestUtils.h"

#include "Animation/Skeleton.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/SkeletalMesh.h"
#include "Misc/PackageName.h"
#include "ReferenceSkeleton.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

namespace PwSkeletonBoneTreeTests
{
    struct FBoneSpec
    {
        const TCHAR* Name;
        int32 ParentIndex;
        EBoneTranslationRetargetingMode::Type Mode;
    };

    inline FString UniquePath(const TCHAR* Prefix)
    {
        return FString::Printf(TEXT("/Game/PinWrightTests/%s_%s"), Prefix,
            *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    }

    inline TArray<FBoneNode>* BoneTreeOf(USkeleton* Skeleton)
    {
        FArrayProperty* Property = FindFProperty<FArrayProperty>(USkeleton::StaticClass(), TEXT("BoneTree"));
        return Property ? Property->ContainerPtrToValuePtr<TArray<FBoneNode>>(Skeleton) : nullptr;
    }

    // Built directly (not through skeleton.add_bone) so the fixture does not depend on the verbs
    // under test. BoneTree is sized by hand because SetBoneTranslationRetargetingMode indexes it
    // unchecked.
    inline USkeleton* MakeSkeleton(const FString& PackagePath, TConstArrayView<FBoneSpec> Bones)
    {
        UPackage* Package = CreatePackage(*PackagePath);
        USkeleton* Skeleton = NewObject<USkeleton>(Package,
            FName(*FPackageName::GetLongPackageAssetName(PackagePath)), RF_Public | RF_Standalone);
        TArray<FBoneNode>* BoneTree = Skeleton ? BoneTreeOf(Skeleton) : nullptr;
        if (!BoneTree)
        {
            return nullptr;
        }
        {
            FReferenceSkeletonModifier Modifier(Skeleton);
            for (const FBoneSpec& Bone : Bones)
            {
                Modifier.Add(FMeshBoneInfo(FName(Bone.Name), Bone.Name, Bone.ParentIndex),
                    FTransform(FVector(0.0, 0.0, 10.0)), Bone.ParentIndex == INDEX_NONE);
            }
        }
        BoneTree->SetNum(Bones.Num());
        for (int32 Index = 0; Index < Bones.Num(); ++Index)
        {
            Skeleton->SetBoneTranslationRetargetingMode(Index, Bones[Index].Mode, false);
        }
        FAssetRegistryModule::AssetCreated(Skeleton);
        return Skeleton;
    }

    inline USkeletalMesh* MakeBoundMesh(const FString& PackagePath, USkeleton* Skeleton, TConstArrayView<FBoneSpec> Bones)
    {
        UPackage* Package = CreatePackage(*PackagePath);
        USkeletalMesh* Mesh = NewObject<USkeletalMesh>(Package,
            FName(*FPackageName::GetLongPackageAssetName(PackagePath)), RF_Public | RF_Standalone);
        if (!Mesh)
        {
            return nullptr;
        }
        Mesh->SetSkeleton(Skeleton);
        {
            FReferenceSkeletonModifier Modifier(Mesh->GetRefSkeleton(), Skeleton);
            for (const FBoneSpec& Bone : Bones)
            {
                Modifier.Add(FMeshBoneInfo(FName(Bone.Name), Bone.Name, Bone.ParentIndex),
                    FTransform(FVector(0.0, 0.0, 10.0)), Bone.ParentIndex == INDEX_NONE);
            }
        }
        FAssetRegistryModule::AssetCreated(Mesh);
        return Mesh;
    }

    inline bool Invoke(FAutomationTestBase& Test, const TCHAR* Method,
        const TSharedPtr<FJsonObject>& Payload, FTestResponseCapture& Capture)
    {
        if (!Test.TestTrue(*FString::Printf(TEXT("%s is registered"), Method),
                InvokeHandlerWithCapture(Method, Payload, Capture)))
        {
            return false;
        }
        return Test.TestTrue(*FString::Printf(TEXT("%s succeeded (errorCode='%s', message='%s')"),
            Method, *Capture.ErrorCode, *Capture.Message), Capture.bSuccess);
    }

    // Every surviving bone keeps its own mode, looked up by name, and BoneTree has exactly one
    // entry per raw bone.
    inline void ExpectModesFollowNames(FAutomationTestBase& Test, USkeleton* Skeleton,
        TConstArrayView<FBoneSpec> Bones)
    {
        const FReferenceSkeleton& RefSkeleton = Skeleton->GetReferenceSkeleton();
        TArray<FBoneNode>* BoneTree = BoneTreeOf(Skeleton);
        Test.TestEqual(TEXT("BoneTree has one entry per reference bone"),
            BoneTree ? BoneTree->Num() : INDEX_NONE, RefSkeleton.GetRawBoneNum());
        for (const FBoneSpec& Bone : Bones)
        {
            const int32 Index = RefSkeleton.FindRawBoneIndex(FName(Bone.Name));
            if (Index == INDEX_NONE)
            {
                continue;
            }
            Test.TestEqual(*FString::Printf(TEXT("bone '%s' keeps its translation retargeting mode"), Bone.Name),
                static_cast<int32>(Skeleton->GetBoneTranslationRetargetingMode(Index)),
                static_cast<int32>(Bone.Mode));
        }
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSkeletonRemoveBoneKeepsRetargetModesTest,
    "PinWright.skeleton.bone_edits.RemoveKeepsRetargetModesByName",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSkeletonRemoveBoneKeepsRetargetModesTest::RunTest(const FString& Parameters)
{
    using namespace PwSkeletonBoneTreeTests;

    // root > a > b > c; removing 'a' reparents 'b' to root and shifts b and c down one slot.
    const FBoneSpec Bones[] = {
        { TEXT("root"), INDEX_NONE, EBoneTranslationRetargetingMode::Animation },
        { TEXT("a"), 0, EBoneTranslationRetargetingMode::Skeleton },
        { TEXT("b"), 1, EBoneTranslationRetargetingMode::AnimationScaled },
        { TEXT("c"), 2, EBoneTranslationRetargetingMode::OrientAndScale },
    };
    const FString SkeletonPath = UniquePath(TEXT("SK_BoneTreeRemove"));
    USkeleton* Skeleton = MakeSkeleton(SkeletonPath, Bones);
    if (!TestNotNull(TEXT("fixture skeleton created"), Skeleton))
    {
        return false;
    }
    TStrongObjectPtr<USkeleton> SkeletonOwner(Skeleton);
    ON_SCOPE_EXIT
    {
        SkeletonOwner.Reset();
        CleanupTestAsset(SkeletonPath);
    };
    ExpectModesFollowNames(*this, Skeleton, Bones);

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("skeletonPath"), Skeleton->GetPathName());
    Payload->SetStringField(TEXT("boneName"), TEXT("a"));
    FTestResponseCapture Capture;
    if (!Invoke(*this, TEXT("skeleton.remove_bone"), Payload, Capture))
    {
        return false;
    }

    TestEqual(TEXT("three bones remain"), Skeleton->GetReferenceSkeleton().GetRawBoneNum(), 3);
    TestEqual(TEXT("'a' is gone"), Skeleton->GetReferenceSkeleton().FindRawBoneIndex(FName(TEXT("a"))), INDEX_NONE);
    ExpectModesFollowNames(*this, Skeleton, Bones);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSkeletonSetBoneParentKeepsRetargetModesTest,
    "PinWright.skeleton.bone_edits.ReparentKeepsRetargetModesByName",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSkeletonSetBoneParentKeepsRetargetModesTest::RunTest(const FString& Parameters)
{
    using namespace PwSkeletonBoneTreeTests;

    // root > a > c, root > b. Parenting 'a' under 'b' must move 'a' (and its child 'c') after 'b'
    // because parents precede children, so every non-root raw index changes.
    const FBoneSpec Bones[] = {
        { TEXT("root"), INDEX_NONE, EBoneTranslationRetargetingMode::Animation },
        { TEXT("a"), 0, EBoneTranslationRetargetingMode::Skeleton },
        { TEXT("b"), 0, EBoneTranslationRetargetingMode::AnimationScaled },
        { TEXT("c"), 1, EBoneTranslationRetargetingMode::OrientAndScale },
    };
    const FString SkeletonPath = UniquePath(TEXT("SK_BoneTreeReparent"));
    USkeleton* Skeleton = MakeSkeleton(SkeletonPath, Bones);
    if (!TestNotNull(TEXT("fixture skeleton created"), Skeleton))
    {
        return false;
    }
    TStrongObjectPtr<USkeleton> SkeletonOwner(Skeleton);
    ON_SCOPE_EXIT
    {
        SkeletonOwner.Reset();
        CleanupTestAsset(SkeletonPath);
    };

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("skeletonPath"), Skeleton->GetPathName());
    Payload->SetStringField(TEXT("boneName"), TEXT("a"));
    Payload->SetStringField(TEXT("parentBone"), TEXT("b"));
    FTestResponseCapture Capture;
    if (!Invoke(*this, TEXT("skeleton.set_bone_parent"), Payload, Capture))
    {
        return false;
    }

    const FReferenceSkeleton& RefSkeleton = Skeleton->GetReferenceSkeleton();
    const int32 AIndex = RefSkeleton.FindRawBoneIndex(FName(TEXT("a")));
    const int32 BIndex = RefSkeleton.FindRawBoneIndex(FName(TEXT("b")));
    // Precondition for this test to mean anything: the edit really reindexed.
    TestEqual(TEXT("'b' moved to raw index 1"), BIndex, 1);
    TestEqual(TEXT("'a' is now parented to 'b'"), RefSkeleton.GetRawParentIndex(AIndex), BIndex);
    ExpectModesFollowNames(*this, Skeleton, Bones);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSkeletonRemoveBoneReportsBoundMeshTest,
    "PinWright.skeleton.bone_edits.RemoveReportsBoundMeshCompatibility",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSkeletonRemoveBoneReportsBoundMeshTest::RunTest(const FString& Parameters)
{
    using namespace PwSkeletonBoneTreeTests;

    const FBoneSpec Bones[] = {
        { TEXT("root"), INDEX_NONE, EBoneTranslationRetargetingMode::Animation },
        { TEXT("a"), 0, EBoneTranslationRetargetingMode::Animation },
        { TEXT("b"), 1, EBoneTranslationRetargetingMode::Animation },
    };
    const FString SkeletonPath = UniquePath(TEXT("SK_BoneTreeBoundMesh"));
    const FString MeshPath = UniquePath(TEXT("SKM_BoneTreeBoundMesh"));
    USkeleton* Skeleton = MakeSkeleton(SkeletonPath, Bones);
    USkeletalMesh* Mesh = Skeleton ? MakeBoundMesh(MeshPath, Skeleton, Bones) : nullptr;
    if (!TestNotNull(TEXT("fixture skeleton created"), Skeleton)
        || !TestNotNull(TEXT("fixture mesh created"), Mesh))
    {
        return false;
    }
    TStrongObjectPtr<USkeleton> SkeletonOwner(Skeleton);
    TStrongObjectPtr<USkeletalMesh> MeshOwner(Mesh);
    ON_SCOPE_EXIT
    {
        MeshOwner.Reset();
        SkeletonOwner.Reset();
        CleanupTestAsset(MeshPath);
        CleanupTestAsset(SkeletonPath);
    };
    TestTrue(TEXT("the mesh matches the skeleton before the edit"), Skeleton->IsCompatibleMesh(Mesh));

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("skeletonPath"), Skeleton->GetPathName());
    Payload->SetStringField(TEXT("boneName"), TEXT("a"));
    FTestResponseCapture Capture;
    if (!Invoke(*this, TEXT("skeleton.remove_bone"), Payload, Capture))
    {
        return false;
    }

    const TArray<TSharedPtr<FJsonValue>>* BoundMeshes = nullptr;
    if (!TestTrue(TEXT("the response carries boundMeshes"),
            Capture.Result.IsValid() && Capture.Result->TryGetArrayField(TEXT("boundMeshes"), BoundMeshes)))
    {
        return false;
    }
    const TSharedPtr<FJsonObject>* MeshEntry = nullptr;
    for (const TSharedPtr<FJsonValue>& Value : *BoundMeshes)
    {
        const TSharedPtr<FJsonObject>* Entry = nullptr;
        FString EntryPath;
        if (Value->TryGetObject(Entry) && (*Entry)->TryGetStringField(TEXT("skeletalMeshPath"), EntryPath)
            && EntryPath == Mesh->GetPathName())
        {
            MeshEntry = Entry;
        }
    }
    if (!TestNotNull(TEXT("boundMeshes names the fixture mesh"), MeshEntry))
    {
        return false;
    }
    bool bCompatible = true;
    TestTrue(TEXT("the entry carries compatible"), (*MeshEntry)->TryGetBoolField(TEXT("compatible"), bCompatible));
    // The mesh still has root > a > b; the skeleton now has root > b, so b's parent chain differs.
    TestFalse(TEXT("the mesh is reported incompatible after its middle bone left the skeleton"), bCompatible);
    return true;
}
