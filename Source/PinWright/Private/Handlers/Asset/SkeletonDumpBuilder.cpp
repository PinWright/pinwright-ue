// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/Asset/SkeletonDumpBuilder.h"

#include "Animation/Skeleton.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/SkeletalMeshSocket.h"
#include "ReferenceSkeleton.h"
#include "Utils/JsonBuilders.h"
#include "Handlers/Asset/AssetDumpHandler.h"
#include "Utils/JsonSidecarRegistry.h"

namespace
{
    using JsonBuilders::BuildTransformJson;
    using JsonBuilders::BuildVectorJson;
    using JsonBuilders::BuildRotatorJson;
}

TSharedPtr<FJsonObject> SkeletonDumpBuilder::BuildSkeletonJson(const USkeleton* Skel)
{
    if (!Skel)
    {
        return nullptr;
    }

    TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();

    // bones[] from the raw reference skeleton (editor-side authoritative copy).
    TArray<TSharedPtr<FJsonValue>> BonesArr;
    const FReferenceSkeleton& RefSkel = Skel->GetReferenceSkeleton();
    const TArray<FMeshBoneInfo>& BoneInfos = RefSkel.GetRawRefBoneInfo();
    const TArray<FTransform>& BonePoses = RefSkel.GetRawRefBonePose();
    const int32 NumBones = BoneInfos.Num();
    BonesArr.Reserve(NumBones);
    for (int32 i = 0; i < NumBones; ++i)
    {
        TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("name"), BoneInfos[i].Name.ToString());
        Entry->SetNumberField(TEXT("parentIndex"), BoneInfos[i].ParentIndex);
        if (BonePoses.IsValidIndex(i))
        {
            Entry->SetObjectField(TEXT("refPose"), BuildTransformJson(BonePoses[i]));
        }
        BonesArr.Add(MakeShared<FJsonValueObject>(Entry));
    }
    Root->SetArrayField(TEXT("bones"), BonesArr);

    Root->SetArrayField(TEXT("virtualBones"), BuildVirtualBonesJson(Skel->GetVirtualBones()));

    TArray<USkeletalMeshSocket*> Sockets;
    Sockets.Reserve(Skel->Sockets.Num());
    for (const TObjectPtr<USkeletalMeshSocket>& SocketPtr : Skel->Sockets)
    {
        Sockets.Add(SocketPtr.Get());
    }
    Root->SetArrayField(TEXT("sockets"), BuildSocketsJson(Sockets));

    // previewMesh: const GetPreviewMesh() returns nullptr if unset.
    Root->SetStringField(TEXT("previewMesh"), JsonBuilders::GetObjectPathSafe(Skel->GetPreviewMesh()));

    return Root;
}

TArray<TSharedPtr<FJsonValue>> SkeletonDumpBuilder::BuildVirtualBonesJson(TConstArrayView<FVirtualBone> VirtualBones)
{
    TArray<TSharedPtr<FJsonValue>> VBonesArr;
    VBonesArr.Reserve(VirtualBones.Num());
    for (const FVirtualBone& VB : VirtualBones)
    {
        TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("name"),   VB.VirtualBoneName.ToString());
        Entry->SetStringField(TEXT("source"), VB.SourceBoneName.ToString());
        Entry->SetStringField(TEXT("target"), VB.TargetBoneName.ToString());
        VBonesArr.Add(MakeShared<FJsonValueObject>(Entry));
    }
    return VBonesArr;
}

TArray<TSharedPtr<FJsonValue>> SkeletonDumpBuilder::BuildSocketsJson(TConstArrayView<USkeletalMeshSocket*> Sockets)
{
    TArray<TSharedPtr<FJsonValue>> SocketsArr;
    SocketsArr.Reserve(Sockets.Num());
    for (const USkeletalMeshSocket* Socket : Sockets)
    {
        if (!Socket)
        {
            continue;
        }
        TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("name"), Socket->SocketName.ToString());
        Entry->SetStringField(TEXT("bone"), Socket->BoneName.ToString());
        // Which asset actually owns this socket. A SkeletalMesh dump reports
        // USkeletalMesh::GetActiveSocketList(), which merges the mesh's own sockets with its
        // Skeleton's and marks neither, so without this field the two are indistinguishable -
        // a reader cannot tell a socket that travels with the mesh from one shared by every
        // mesh bound to that skeleton. Editing the wrong one silently changes every other mesh.
        Entry->SetStringField(TEXT("ownedBy"), JsonBuilders::GetObjectPathSafe(Socket->GetOuter()));
        Entry->SetObjectField(TEXT("location"), BuildVectorJson(Socket->RelativeLocation));
        Entry->SetObjectField(TEXT("rotation"), BuildRotatorJson(Socket->RelativeRotation));
        Entry->SetObjectField(TEXT("scale"),    BuildVectorJson(Socket->RelativeScale));
        SocketsArr.Add(MakeShared<FJsonValueObject>(Entry));
    }
    return SocketsArr;
}

namespace
{
    UClass* GetSkeletonSidecarClass()
    {
        return USkeleton::StaticClass();
    }

    TSharedPtr<FJsonObject> BuildSkeletonSidecar(UObject* Asset)
    {
        return SkeletonDumpBuilder::BuildSkeletonJson(Cast<USkeleton>(Asset));
    }
}

REGISTER_DUMP_JSON_SIDECAR(TEXT("skeleton"), DumpFileNames::Skeleton,
    &GetSkeletonSidecarClass, &BuildSkeletonSidecar,
    nullptr, nullptr, nullptr, 100);
