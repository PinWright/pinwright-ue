// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"
#include "Handlers/Asset/SkeletalMeshDumpBuilder.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Tests/TestUtils.h"


#include "Animation/Skeleton.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/SkeletalMeshSocket.h"
#include "ReferenceSkeleton.h"
#include "UObject/Package.h"

namespace
{
    TSharedPtr<FJsonObject> FindObjectByStringField(
        const TArray<TSharedPtr<FJsonValue>>* Values,
        const FString& FieldName,
        const FString& ExpectedValue)
    {
        if (!Values)
        {
            return nullptr;
        }

        for (const TSharedPtr<FJsonValue>& Value : *Values)
        {
            const TSharedPtr<FJsonObject>* Object = nullptr;
            if (Value.IsValid()
                && Value->TryGetObject(Object)
                && Object
                && Object->IsValid()
                && (*Object)->GetStringField(FieldName) == ExpectedValue)
            {
                return *Object;
            }
        }

        return nullptr;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSkeletalMeshDumpBuilderEmitsTest,
    "PinWright.AssetDump.SkeletalMeshBuilderEmits",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSkeletalMeshDumpBuilderEmitsTest::RunTest(const FString& Parameters)
{
    USkeletalMesh* Mesh = NewObject<USkeletalMesh>(GetTransientPackage());
    TestNotNull(TEXT("Transient SkeletalMesh created"), Mesh);
    if (!Mesh)
    {
        return false;
    }

    TSharedPtr<FJsonObject> Result = SkeletalMeshDumpBuilder::BuildSkeletalMeshJson(Mesh);
    TestTrue(TEXT("Result is valid for non-null mesh"), Result.IsValid());
    if (!Result.IsValid())
    {
        return false;
    }

    // bounds — present and an object with origin/extent/sphereRadius.
    const TSharedPtr<FJsonObject>* BoundsObj = nullptr;
    TestTrue(TEXT("bounds is an object"), Result->TryGetObjectField(TEXT("bounds"), BoundsObj));
    if (BoundsObj && BoundsObj->IsValid())
    {
        TestTrue(TEXT("bounds.origin present"), (*BoundsObj)->HasTypedField<EJson::Object>(TEXT("origin")));
        TestTrue(TEXT("bounds.extent present"), (*BoundsObj)->HasTypedField<EJson::Object>(TEXT("extent")));
        TestTrue(TEXT("bounds.sphereRadius present"), (*BoundsObj)->HasTypedField<EJson::Number>(TEXT("sphereRadius")));
    }

    // materials — empty array.
    const TArray<TSharedPtr<FJsonValue>>* Materials = nullptr;
    TestTrue(TEXT("materials is an array"), Result->TryGetArrayField(TEXT("materials"), Materials));
    TestTrue(TEXT("materials is empty"), Materials && Materials->Num() == 0);

    // lods — numeric, equals 0.
    TestTrue(TEXT("lods is a number"), Result->HasTypedField<EJson::Number>(TEXT("lods")));
    TestEqual(TEXT("lods == 0"), static_cast<int32>(Result->GetNumberField(TEXT("lods"))), 0);

    // Per-LOD arrays — present, empty.
    const TArray<TSharedPtr<FJsonValue>>* TrianglesByLod = nullptr;
    const TArray<TSharedPtr<FJsonValue>>* VerticesByLod = nullptr;
    const TArray<TSharedPtr<FJsonValue>>* SectionsByLod = nullptr;
    const TArray<TSharedPtr<FJsonValue>>* NumTexCoordsByLod = nullptr;
    TestTrue(TEXT("trianglesByLod is an array"), Result->TryGetArrayField(TEXT("trianglesByLod"), TrianglesByLod));
    TestTrue(TEXT("verticesByLod is an array"), Result->TryGetArrayField(TEXT("verticesByLod"), VerticesByLod));
    TestTrue(TEXT("sectionsByLod is an array"), Result->TryGetArrayField(TEXT("sectionsByLod"), SectionsByLod));
    TestTrue(TEXT("numTexCoordsByLod is an array"), Result->TryGetArrayField(TEXT("numTexCoordsByLod"), NumTexCoordsByLod));
    TestTrue(TEXT("trianglesByLod is empty"), TrianglesByLod && TrianglesByLod->Num() == 0);
    TestTrue(TEXT("verticesByLod is empty"), VerticesByLod && VerticesByLod->Num() == 0);
    TestTrue(TEXT("sectionsByLod is empty"), SectionsByLod && SectionsByLod->Num() == 0);
    TestTrue(TEXT("numTexCoordsByLod is empty"), NumTexCoordsByLod && NumTexCoordsByLod->Num() == 0);

    // physicsAsset / skeleton — empty strings.
    TestTrue(TEXT("physicsAsset is a string"), Result->HasTypedField<EJson::String>(TEXT("physicsAsset")));
    TestEqual(TEXT("physicsAsset is empty"), Result->GetStringField(TEXT("physicsAsset")), FString());
    TestTrue(TEXT("skeleton is a string"), Result->HasTypedField<EJson::String>(TEXT("skeleton")));
    TestEqual(TEXT("skeleton is empty"), Result->GetStringField(TEXT("skeleton")), FString());

    const TArray<TSharedPtr<FJsonValue>>* Sockets = nullptr;
    const TArray<TSharedPtr<FJsonValue>>* VirtualBones = nullptr;
    TestTrue(TEXT("sockets is an array"), Result->TryGetArrayField(TEXT("sockets"), Sockets));
    TestTrue(TEXT("virtualBones is an array"), Result->TryGetArrayField(TEXT("virtualBones"), VirtualBones));
    TestTrue(TEXT("sockets is empty"), Sockets && Sockets->Num() == 0);
    TestTrue(TEXT("virtualBones is empty"), VirtualBones && VirtualBones->Num() == 0);

    // Counterfactual: null guard fires.
    TSharedPtr<FJsonObject> NullResult = SkeletalMeshDumpBuilder::BuildSkeletalMeshJson(nullptr);
    TestFalse(TEXT("Null mesh returns invalid pointer"), NullResult.IsValid());

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSkeletalMeshDumpBuilderEmitsActiveSocketsAndVirtualBonesTest,
    "PinWright.AssetDump.SkeletalMeshBuilderEmitsActiveSocketsAndVirtualBones",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSkeletalMeshDumpBuilderEmitsActiveSocketsAndVirtualBonesTest::RunTest(const FString& Parameters)
{
    USkeletalMesh* Mesh = NewObject<USkeletalMesh>(GetTransientPackage());
    USkeleton* Skeleton = NewObject<USkeleton>(GetTransientPackage());
    TestNotNull(TEXT("Transient SkeletalMesh created"), Mesh);
    TestNotNull(TEXT("Transient Skeleton created"), Skeleton);
    if (!Mesh || !Skeleton)
    {
        return false;
    }

    Mesh->SetSkeleton(Skeleton);

    {
        FReferenceSkeletonModifier Modifier(Skeleton);
        Modifier.Add(FMeshBoneInfo(FName(TEXT("root")), TEXT("root"), INDEX_NONE), FTransform::Identity, true);
        Modifier.Add(FMeshBoneInfo(FName(TEXT("camera")), TEXT("camera"), 0), FTransform::Identity);
    }

    // USkeleton::AddNewVirtualBone()'s HandleVirtualBoneChanges() walks every USkeletalMesh
    // bound to this skeleton via TObjectIterator and rebuilds *each mesh's own* ref skeleton.
    // If the mesh's ref skeleton has zero raw bones, FReferenceSkeleton::RebuildRefSkeleton
    // dereferences ComponentSpaceFlags[0] on an empty array (Engine/ReferenceSkeleton.cpp:506)
    // and the assertion in Array.h fires. Mirror the skeleton's bones onto the mesh ref
    // skeleton so the rebuild has a valid root to project virtual bones from.
    {
        FReferenceSkeletonModifier MeshModifier(Mesh->GetRefSkeleton(), Skeleton);
        MeshModifier.Add(FMeshBoneInfo(FName(TEXT("root")), TEXT("root"), INDEX_NONE), FTransform::Identity, true);
        MeshModifier.Add(FMeshBoneInfo(FName(TEXT("camera")), TEXT("camera"), 0), FTransform::Identity);
    }

    USkeletalMeshSocket* SkeletonSocket = NewObject<USkeletalMeshSocket>(Skeleton);
    SkeletonSocket->SocketName = TEXT("SkeletonSocket");
    SkeletonSocket->BoneName = TEXT("root");
    Skeleton->Sockets.Add(SkeletonSocket);

    USkeletalMeshSocket* MeshOnlySocket = NewObject<USkeletalMeshSocket>(Mesh);
    MeshOnlySocket->SocketName = TEXT("MeshOnlySocket");
    MeshOnlySocket->BoneName = TEXT("root");
    Mesh->GetMeshOnlySocketList().Add(MeshOnlySocket);

    FName VirtualBoneName;
    TestTrue(TEXT("Virtual bone added"), Skeleton->AddNewVirtualBone(FName(TEXT("root")), FName(TEXT("camera")), VirtualBoneName));

    TSharedPtr<FJsonObject> Result = SkeletalMeshDumpBuilder::BuildSkeletalMeshJson(Mesh);
    TestTrue(TEXT("Result is valid for mesh with skeleton"), Result.IsValid());
    if (!Result.IsValid())
    {
        return false;
    }

    TestFalse(TEXT("skeleton path is populated"), Result->GetStringField(TEXT("skeleton")).IsEmpty());

    const TArray<TSharedPtr<FJsonValue>>* Sockets = nullptr;
    TestTrue(TEXT("sockets is an array"), Result->TryGetArrayField(TEXT("sockets"), Sockets));
    TestEqual(TEXT("active sockets include skeleton and mesh-only sockets"), Sockets ? Sockets->Num() : 0, 2);

    const TSharedPtr<FJsonObject> SkeletonSocketJson = FindObjectByStringField(Sockets, TEXT("name"), TEXT("SkeletonSocket"));
    const TSharedPtr<FJsonObject> MeshOnlySocketJson = FindObjectByStringField(Sockets, TEXT("name"), TEXT("MeshOnlySocket"));
    TestTrue(TEXT("skeleton socket emitted"), SkeletonSocketJson.IsValid());
    TestTrue(TEXT("mesh-only socket emitted"), MeshOnlySocketJson.IsValid());
    if (SkeletonSocketJson.IsValid())
    {
        TestEqual(TEXT("skeleton socket bone emitted"), SkeletonSocketJson->GetStringField(TEXT("bone")), FString(TEXT("root")));
        // The whole point of ownedBy: the merged array is otherwise indistinguishable, and a
        // reader cannot tell a socket that travels with this mesh from one shared by every mesh
        // bound to the skeleton. Asserting the two differ is what makes the field load-bearing -
        // an ownedBy that reported the mesh for both entries would pass a presence-only check.
        TestEqual(TEXT("skeleton socket is attributed to the skeleton"),
            SkeletonSocketJson->GetStringField(TEXT("ownedBy")), Skeleton->GetPathName());
    }
    if (MeshOnlySocketJson.IsValid())
    {
        TestEqual(TEXT("mesh-only socket bone emitted"), MeshOnlySocketJson->GetStringField(TEXT("bone")), FString(TEXT("root")));
        TestEqual(TEXT("mesh-only socket is attributed to the mesh"),
            MeshOnlySocketJson->GetStringField(TEXT("ownedBy")), Mesh->GetPathName());
    }
    if (SkeletonSocketJson.IsValid() && MeshOnlySocketJson.IsValid())
    {
        TestNotEqual(TEXT("the two owners are different assets"),
            SkeletonSocketJson->GetStringField(TEXT("ownedBy")),
            MeshOnlySocketJson->GetStringField(TEXT("ownedBy")));
    }

    const TArray<TSharedPtr<FJsonValue>>* VirtualBones = nullptr;
    TestTrue(TEXT("virtualBones is an array"), Result->TryGetArrayField(TEXT("virtualBones"), VirtualBones));
    TestEqual(TEXT("virtual bone count"), VirtualBones ? VirtualBones->Num() : 0, 1);

    const TSharedPtr<FJsonObject> VirtualBoneJson = FindObjectByStringField(VirtualBones, TEXT("name"), VirtualBoneName.ToString());
    TestTrue(TEXT("virtual bone emitted"), VirtualBoneJson.IsValid());
    if (VirtualBoneJson.IsValid())
    {
        TestEqual(TEXT("virtual bone source emitted"), VirtualBoneJson->GetStringField(TEXT("source")), FString(TEXT("root")));
        TestEqual(TEXT("virtual bone target emitted"), VirtualBoneJson->GetStringField(TEXT("target")), FString(TEXT("camera")));
    }

    return true;
}
