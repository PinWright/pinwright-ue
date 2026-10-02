// Copyright (c) 2026 Alexander Penkin. MIT License.

// skeleton.edit_mesh_bones and skeleton.create_skeleton fromSkeletalMesh, on copies of
// /Engine/EngineMeshes/SkeletalCube under /Game/PinWrightTests.
//
// The skeleton.add_bone family edits only the USkeleton, so a bone added that way never reaches the
// mesh's reference skeleton. These tests read the MESH back: a fix that only touched the Skeleton,
// or that let the engine's modal merge dialog decide an incompatible edit, fails here.

#include "Misc/AutomationTest.h"

#include "Animation/Skeleton.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EditorAssetLibrary.h"
#include "Engine/SkeletalMesh.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "ReferenceSkeleton.h"
#include "Rendering/SkeletalMeshLODRenderData.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "SkinnedAssetCompiler.h"
#include "Tests/TestSkipReporting.h"
#include "Tests/TestUtils.h"
#include "UObject/Package.h"

namespace PwEditMeshBonesTests
{
    const TCHAR* const EngineMesh = TEXT("/Engine/EngineMeshes/SkeletalCube");
    const TCHAR* const EngineSkeleton = TEXT("/Engine/EngineMeshes/SkeletalCube_Skeleton");

    // A private folder per test; the destructor removes everything created in it, including assets
    // the verb under test created.
    struct FFixture
    {
        FString Folder = FString::Printf(TEXT("/Game/PinWrightTests/PWMeshBones_%s"),
            *FGuid::NewGuid().ToString(EGuidFormats::Digits));
        USkeletalMesh* Mesh = nullptr;
        USkeleton* Skeleton = nullptr;

        ~FFixture()
        {
            TArray<FAssetData> Found;
            IAssetRegistry::GetChecked().GetAssetsByPath(FName(*Folder), Found, /*bRecursive=*/true);
            TSet<FString> Packages = { Folder / TEXT("SKM_Cube"), Folder / TEXT("SK_Cube") };
            for (const FAssetData& Data : Found)
            {
                Packages.Add(Data.PackageName.ToString());
            }
            for (const FString& Package : Packages)
            {
                CleanupTestAsset(Package);
            }
        }

        // False (skip marker or error recorded) when the copies cannot be made. bOwnSkeleton binds
        // the copied mesh to a copied Skeleton; otherwise it stays on the engine Skeleton.
        bool Build(FAutomationTestBase& Test, bool bOwnSkeleton)
        {
            if (!UEditorAssetLibrary::DoesAssetExist(EngineMesh))
            {
                PinWrightTestSkip::SkipAssertions(Test, TEXT("engine-skeletal-cube-unavailable"),
                    TEXT("Skipped: /Engine/EngineMeshes/SkeletalCube unavailable."));
                return false;
            }
            Mesh = Cast<USkeletalMesh>(UEditorAssetLibrary::DuplicateAsset(EngineMesh, Folder / TEXT("SKM_Cube")));
            if (bOwnSkeleton)
            {
                Skeleton = Cast<USkeleton>(UEditorAssetLibrary::DuplicateAsset(EngineSkeleton, Folder / TEXT("SK_Cube")));
                if (Mesh && Skeleton)
                {
                    Mesh->SetSkeleton(Skeleton);
                }
            }
            return Test.TestTrue(TEXT("fixture copies made"), Mesh && (!bOwnSkeleton || Skeleton));
        }
    };

    FName ParentName(const FReferenceSkeleton& Ref, FName Bone)
    {
        const int32 Index = Ref.FindRawBoneIndex(Bone);
        const int32 Parent = Index == INDEX_NONE ? INDEX_NONE : Ref.GetRawParentIndex(Index);
        return Parent == INDEX_NONE ? NAME_None : Ref.GetBoneName(Parent);
    }

    TSharedPtr<FJsonObject> MakeOp(const TCHAR* Op, FName Bone, FName Parent)
    {
        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("op"), Op);
        Entry->SetStringField(TEXT("bone"), Bone.ToString());
        if (!Parent.IsNone())
        {
            Entry->SetStringField(TEXT("parent"), Parent.ToString());
        }
        return Entry;
    }

    void EditMeshBones(USkeletalMesh* Mesh, const TArray<TSharedPtr<FJsonObject>>& Ops, bool bDryRun,
        FTestResponseCapture& Capture)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("skeletalMeshPath"), Mesh->GetPathName());
        TArray<TSharedPtr<FJsonValue>> OpValues;
        for (const TSharedPtr<FJsonObject>& Op : Ops)
        {
            OpValues.Add(MakeShared<FJsonValueObject>(Op));
        }
        Payload->SetArrayField(TEXT("ops"), OpValues);
        Payload->SetBoolField(TEXT("dryRun"), bDryRun);
        InvokeHandlerWithCapture(TEXT("skeleton.edit_mesh_bones"), Payload, Capture);
    }

    bool ResponseListsBone(const FTestResponseCapture& Capture, const FString& Bone)
    {
        const TArray<TSharedPtr<FJsonValue>>* Bones = nullptr;
        if (!Capture.Result.IsValid() || !Capture.Result->TryGetArrayField(TEXT("bones"), Bones))
        {
            return false;
        }
        for (const TSharedPtr<FJsonValue>& Value : *Bones)
        {
            const TSharedPtr<FJsonObject>* Entry = nullptr;
            FString Name;
            if (Value->TryGetObject(Entry) && (*Entry)->TryGetStringField(TEXT("name"), Name) && Name == Bone)
            {
                return true;
            }
        }
        return false;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSkeletonEditMeshBonesAddLeafTest,
    "PinWright.skeleton.edit_mesh_bones.AddLeafBoneReachesMeshAndSkeleton",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSkeletonEditMeshBonesAddLeafTest::RunTest(const FString& Parameters)
{
    using namespace PwEditMeshBonesTests;
    FFixture Fixture;
    if (!Fixture.Build(*this, /*bOwnSkeleton=*/true))
    {
        return true;
    }
    USkeletalMesh* Mesh = Fixture.Mesh;
    const FReferenceSkeleton& MeshRef = Mesh->GetRefSkeleton();
    const FName Leaf = MeshRef.GetBoneName(MeshRef.GetRawBoneNum() - 1);
    const int32 BonesBefore = MeshRef.GetRawBoneNum();
    FSkinnedAssetCompilingManager::Get().FinishCompilation({ Mesh });
    const FSkeletalMeshRenderData* RenderBefore = Mesh->GetResourceForRendering();
    const int32 VerticesBefore = RenderBefore && RenderBefore->LODRenderData.Num() > 0
        ? static_cast<int32>(RenderBefore->LODRenderData[0].GetNumVertices()) : 0;

    TSharedPtr<FJsonObject> Add = MakeOp(TEXT("add"), FName(TEXT("pw_weapon")), Leaf);
    TSharedPtr<FJsonObject> Location = MakeShared<FJsonObject>();
    Location->SetNumberField(TEXT("x"), 0.0);
    Location->SetNumberField(TEXT("y"), 0.0);
    Location->SetNumberField(TEXT("z"), 5.0);
    Add->SetObjectField(TEXT("location"), Location);
    FTestResponseCapture Capture;
    EditMeshBones(Mesh, { Add }, /*bDryRun=*/false, Capture);
    if (!TestTrue(FString::Printf(TEXT("edit_mesh_bones succeeded (errorCode='%s', message='%s')"), *Capture.ErrorCode, *Capture.Message),
            Capture.bSuccess))
    {
        return false;
    }

    // The mesh's own reference skeleton is the thing the skeleton.* verbs never reached.
    TestEqual(TEXT("the mesh gained one bone"), Mesh->GetRefSkeleton().GetRawBoneNum(), BonesBefore + 1);
    TestEqual(TEXT("the mesh's new bone hangs off the leaf"), ParentName(Mesh->GetRefSkeleton(), FName(TEXT("pw_weapon"))), Leaf);
    TestEqual(TEXT("the Skeleton's new bone hangs off the leaf"),
        ParentName(Fixture.Skeleton->GetReferenceSkeleton(), FName(TEXT("pw_weapon"))), Leaf);
    TestTrue(TEXT("the Skeleton still accepts the mesh"), Fixture.Skeleton->IsCompatibleMesh(Mesh));
    TestTrue(TEXT("the response's measured bone list names the new bone"), ResponseListsBone(Capture, TEXT("pw_weapon")));

    FSkinnedAssetCompilingManager::Get().FinishCompilation({ Mesh });
    const FSkeletalMeshRenderData* RenderAfter = Mesh->GetResourceForRendering();
    const int32 VerticesAfter = RenderAfter && RenderAfter->LODRenderData.Num() > 0
        ? static_cast<int32>(RenderAfter->LODRenderData[0].GetNumVertices()) : 0;
    TestTrue(TEXT("the mesh had render data before the edit"), VerticesBefore > 0);
    TestEqual(TEXT("the rebuilt mesh still renders every vertex"), VerticesAfter, VerticesBefore);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSkeletonEditMeshBonesIncompatibleTest,
    "PinWright.skeleton.edit_mesh_bones.IncompatibleReparentIsRefused",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSkeletonEditMeshBonesIncompatibleTest::RunTest(const FString& Parameters)
{
    using namespace PwEditMeshBonesTests;
    FFixture Fixture;
    if (!Fixture.Build(*this, /*bOwnSkeleton=*/true))
    {
        return true;
    }
    USkeletalMesh* Mesh = Fixture.Mesh;
    const FReferenceSkeleton& MeshRef = Mesh->GetRefSkeleton();
    if (MeshRef.GetRawBoneNum() < 2)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("engine-skeletal-cube-single-bone"),
            TEXT("Skipped: SkeletalCube has a single bone; nothing to reparent."));
        return true;
    }
    const FName Root = MeshRef.GetBoneName(0);
    const FName Leaf = MeshRef.GetBoneName(MeshRef.GetRawBoneNum() - 1);
    const FName LeafParentBefore = ParentName(MeshRef, Leaf);
    const int32 BonesBefore = MeshRef.GetRawBoneNum();

    // The Skeleton keeps Leaf under its old parent; on the mesh it would sit under a new bone, so the
    // parent chains differ: the engine's commit would open its modal merge dialog here.
    FTestResponseCapture Capture;
    EditMeshBones(Mesh, { MakeOp(TEXT("add"), FName(TEXT("pw_mid")), Root), MakeOp(TEXT("reparent"), Leaf, FName(TEXT("pw_mid"))) },
        /*bDryRun=*/false, Capture);

    TestFalse(TEXT("the incompatible batch is refused"), Capture.bSuccess);
    TestEqual(TEXT("refused with SKELETON_EDIT_INCOMPATIBLE"), Capture.ErrorCode, FString(TEXT("SKELETON_EDIT_INCOMPATIBLE")));
    TestTrue(TEXT("the refusal names the offending bone"), Capture.Message.Contains(Leaf.ToString()));
    TestEqual(TEXT("the mesh kept its bones"), Mesh->GetRefSkeleton().GetRawBoneNum(), BonesBefore);
    TestEqual(TEXT("the mesh's leaf kept its parent"), ParentName(Mesh->GetRefSkeleton(), Leaf), LeafParentBefore);
    TestEqual(TEXT("the add that preceded the refused op was not applied"),
        Mesh->GetRefSkeleton().FindRawBoneIndex(FName(TEXT("pw_mid"))), INDEX_NONE);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSkeletonEditMeshBonesDryRunTest,
    "PinWright.skeleton.edit_mesh_bones.DryRunWritesNothing",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSkeletonEditMeshBonesDryRunTest::RunTest(const FString& Parameters)
{
    using namespace PwEditMeshBonesTests;
    FFixture Fixture;
    if (!Fixture.Build(*this, /*bOwnSkeleton=*/true))
    {
        return true;
    }
    USkeletalMesh* Mesh = Fixture.Mesh;
    const FName Root = Mesh->GetRefSkeleton().GetBoneName(0);
    const int32 MeshBonesBefore = Mesh->GetRefSkeleton().GetRawBoneNum();
    const int32 SkeletonBonesBefore = Fixture.Skeleton->GetReferenceSkeleton().GetRawBoneNum();
    Mesh->GetOutermost()->SetDirtyFlag(false);
    Fixture.Skeleton->GetOutermost()->SetDirtyFlag(false);

    FTestResponseCapture Capture;
    EditMeshBones(Mesh, { MakeOp(TEXT("add"), FName(TEXT("pw_dry")), Root) }, /*bDryRun=*/true, Capture);
    if (!TestTrue(FString::Printf(TEXT("dry run succeeded (errorCode='%s', message='%s')"), *Capture.ErrorCode, *Capture.Message),
            Capture.bSuccess))
    {
        return false;
    }
    bool bCommitted = true;
    TestTrue(TEXT("the response carries committed"), Capture.Result->TryGetBoolField(TEXT("committed"), bCommitted));
    TestFalse(TEXT("nothing was committed"), bCommitted);
    TestTrue(TEXT("the predicted bone list names the new bone"), ResponseListsBone(Capture, TEXT("pw_dry")));
    TestEqual(TEXT("the mesh kept its bones"), Mesh->GetRefSkeleton().GetRawBoneNum(), MeshBonesBefore);
    TestEqual(TEXT("the Skeleton kept its bones"), Fixture.Skeleton->GetReferenceSkeleton().GetRawBoneNum(), SkeletonBonesBefore);
    TestFalse(TEXT("the mesh package is still clean"), Mesh->GetOutermost()->IsDirty());
    TestFalse(TEXT("the Skeleton package is still clean"), Fixture.Skeleton->GetOutermost()->IsDirty());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSkeletonCreateFromSkeletalMeshTest,
    "PinWright.skeleton.create_skeleton.FromSkeletalMeshCopiesHierarchy",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSkeletonCreateFromSkeletalMeshTest::RunTest(const FString& Parameters)
{
    using namespace PwEditMeshBonesTests;
    FFixture Fixture;
    if (!Fixture.Build(*this, /*bOwnSkeleton=*/false))
    {
        return true;
    }
    USkeletalMesh* Mesh = Fixture.Mesh;
    const USkeleton* EngineSkeletonBefore = Mesh->GetSkeleton();

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("path"), Fixture.Folder / TEXT("SK_FromMesh"));
    Payload->SetStringField(TEXT("fromSkeletalMesh"), Mesh->GetPathName());
    FTestResponseCapture Capture;
    InvokeHandlerWithCapture(TEXT("skeleton.create_skeleton"), Payload, Capture);
    if (!TestTrue(FString::Printf(TEXT("create_skeleton succeeded (errorCode='%s', message='%s')"), *Capture.ErrorCode, *Capture.Message),
            Capture.bSuccess))
    {
        return false;
    }

    const FString NewPath = Fixture.Folder / TEXT("SK_FromMesh");
    USkeleton* NewSkeleton = FindObject<USkeleton>(nullptr, *(NewPath + TEXT(".SK_FromMesh")));
    if (!TestNotNull(TEXT("the new Skeleton exists"), NewSkeleton))
    {
        return false;
    }
    TestTrue(TEXT("the mesh now references the new Skeleton"), Mesh->GetSkeleton() == NewSkeleton);
    TestTrue(TEXT("the mesh was not left on its previous Skeleton"), Mesh->GetSkeleton() != EngineSkeletonBefore);

    const FReferenceSkeleton& MeshRef = Mesh->GetRefSkeleton();
    const FReferenceSkeleton& NewRef = NewSkeleton->GetReferenceSkeleton();
    if (!TestEqual(TEXT("same bone count"), NewRef.GetRawBoneNum(), MeshRef.GetRawBoneNum()))
    {
        return false;
    }
    for (int32 Index = 0; Index < MeshRef.GetRawBoneNum(); ++Index)
    {
        const FName Bone = MeshRef.GetBoneName(Index);
        TestEqual(FString::Printf(TEXT("bone %d has the mesh's name"), Index), NewRef.GetBoneName(Index), Bone);
        TestEqual(FString::Printf(TEXT("bone '%s' has the mesh's parent"), *Bone.ToString()), ParentName(NewRef, Bone), ParentName(MeshRef, Bone));
    }
    TestTrue(TEXT("the new Skeleton accepts the mesh"), NewSkeleton->IsCompatibleMesh(Mesh));
    return true;
}
