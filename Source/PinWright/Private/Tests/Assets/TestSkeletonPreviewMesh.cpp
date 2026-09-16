// Copyright (c) 2026 Alexander Penkin. MIT License.

// skeleton.set_preview_mesh. A USkeleton with no preview mesh opens with an empty viewport,
// and nothing in the plugin used to fill that slot.
//
// The interesting half is the refusal. USkeleton::SetPreviewMesh validates nothing, but the
// non-const USkeleton::GetPreviewMesh runs IsCompatibleForEditor and Reset()s the stored
// pointer when it fails -- without dirtying anything. So a mismatched mesh stores, saves, and
// is silently erased by the next reader: a green report the engine undoes. These tests pin
// both directions, because either assertion alone is satisfied by a one-line change that
// recreates the other defect.
#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/Guid.h"
#include "Misc/ScopeExit.h"
#include "Tests/TestSkipReporting.h"
#include "Tests/TestUtils.h"

#include "Animation/Skeleton.h"
#include "Animation/MorphTarget.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/SkeletalMeshSocket.h"
#include "Misc/PackageName.h"
#include "PhysicsEngine/BodySetup.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "PhysicsEngine/PhysicsConstraintTemplate.h"
#if __has_include("PhysicsEngine/SkeletalBodySetup.h")
#include "PhysicsEngine/SkeletalBodySetup.h"
#endif
#include "ReferenceSkeleton.h"
#include "Rendering/SkeletalMeshLODModel.h"
#include "UObject/Package.h"

namespace PwSkeletonPreviewMeshTests
{
    inline FString UniquePath(const TCHAR* Prefix)
    {
        return FString::Printf(TEXT("/Game/PinWrightTests/%s_%s"), Prefix,
            *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    }

    // A skeleton reachable by asset path, because the handler resolves its arguments through
    // StaticLoadObject rather than taking an object pointer.
    inline USkeleton* MakeSkeleton(const FString& PackagePath)
    {
        UPackage* Package = CreatePackage(*PackagePath);
        const FString AssetName = FPackageName::GetLongPackageAssetName(PackagePath);
        USkeleton* Skeleton = NewObject<USkeleton>(
            Package, FName(*AssetName), RF_Public | RF_Standalone);
        if (!Skeleton)
        {
            return nullptr;
        }

        FReferenceSkeletonModifier Modifier(Skeleton);
        FMeshBoneInfo BoneInfo;
        BoneInfo.Name = FName(TEXT("root"));
        BoneInfo.ExportName = TEXT("root");
        BoneInfo.ParentIndex = INDEX_NONE;
        Modifier.Add(BoneInfo, FTransform::Identity, /*bAllowMultipleRoots=*/true);
        return Skeleton;
    }

    inline USkeletalMesh* MakeMesh(const FString& PackagePath, USkeleton* BoundSkeleton)
    {
        UPackage* Package = CreatePackage(*PackagePath);
        const FString AssetName = FPackageName::GetLongPackageAssetName(PackagePath);
        USkeletalMesh* Mesh = NewObject<USkeletalMesh>(
            Package, FName(*AssetName), RF_Public | RF_Standalone);
        if (Mesh && BoundSkeleton)
        {
            Mesh->SetSkeleton(BoundSkeleton);
        }
        return Mesh;
    }

    inline USkeleton* MakeTwoBoneSkeleton(const FString& PackagePath)
    {
        USkeleton* Skeleton = MakeSkeleton(PackagePath);
        if (!Skeleton)
        {
            return nullptr;
        }

        FReferenceSkeletonModifier Modifier(Skeleton);
        FMeshBoneInfo BoneInfo;
        BoneInfo.Name = FName(TEXT("child"));
        BoneInfo.ExportName = TEXT("child");
        BoneInfo.ParentIndex = 0;
        Modifier.Add(BoneInfo, FTransform(FRotator::ZeroRotator, FVector(0.0f, 0.0f, 10.0f)), false);
        return Skeleton;
    }

    inline UPhysicsAsset* MakePhysicsAsset(const FString& PackagePath)
    {
        UPackage* Package = CreatePackage(*PackagePath);
        const FString AssetName = FPackageName::GetLongPackageAssetName(PackagePath);
        UPhysicsAsset* PhysicsAsset = NewObject<UPhysicsAsset>(
            Package, FName(*AssetName), RF_Public | RF_Standalone | RF_Transactional);
        if (!PhysicsAsset)
        {
            return nullptr;
        }

        for (const FName BoneName : { FName(TEXT("root")), FName(TEXT("child")) })
        {
            USkeletalBodySetup* Body = NewObject<USkeletalBodySetup>(PhysicsAsset, NAME_None, RF_Transactional);
            if (!Body)
            {
                return nullptr;
            }
            Body->BoneName = BoneName;
            FKSphereElem Sphere;
            Sphere.Radius = 1.0f;
            Body->AggGeom.SphereElems.Add(Sphere);
            PhysicsAsset->SkeletalBodySetups.Add(Body);
        }

        UPhysicsConstraintTemplate* Constraint = NewObject<UPhysicsConstraintTemplate>(
            PhysicsAsset, NAME_None, RF_Transactional);
        if (!Constraint)
        {
            return nullptr;
        }
        Constraint->DefaultInstance.ConstraintBone1 = FName(TEXT("root"));
        Constraint->DefaultInstance.ConstraintBone2 = FName(TEXT("child"));
        PhysicsAsset->ConstraintSetup.Add(Constraint);
        PhysicsAsset->UpdateBodySetupIndexMap();
        PhysicsAsset->UpdateBoundsBodiesArray();
        return PhysicsAsset;
    }

    inline FString ReadString(const TSharedPtr<FJsonObject>& Result, const TCHAR* Field)
    {
        FString Value;
        return (Result.IsValid() && Result->TryGetStringField(Field, Value)) ? Value : FString();
    }

    inline bool ReadBool(const TSharedPtr<FJsonObject>& Result, const TCHAR* Field, bool bFallback)
    {
        bool bValue = bFallback;
        return (Result.IsValid() && Result->TryGetBoolField(Field, bValue)) ? bValue : bFallback;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSkeletonSetPreviewMeshAppliesTest,
    "PinWright.skeleton.set_preview_mesh.FillsTheSlotAndReportsMarkDirtyHonestly",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSkeletonSetPreviewMeshAppliesTest::RunTest(const FString& Parameters)
{
    using namespace PwSkeletonPreviewMeshTests;

    const FString SkeletonPath = UniquePath(TEXT("SK_PreviewSlot"));
    const FString MeshPath = UniquePath(TEXT("SKM_PreviewSlot"));

    USkeleton* Skeleton = MakeSkeleton(SkeletonPath);
    if (!TestNotNull(TEXT("probe skeleton created"), Skeleton))
    {
        return false;
    }
    USkeletalMesh* Mesh = MakeMesh(MeshPath, Skeleton);
    if (!TestNotNull(TEXT("probe mesh created"), Mesh))
    {
        return false;
    }

    Skeleton->AddToRoot();
    Mesh->AddToRoot();
    ON_SCOPE_EXIT
    {
        Mesh->RemoveFromRoot();
        Skeleton->RemoveFromRoot();
        CleanupTestAsset(MeshPath);
        CleanupTestAsset(SkeletonPath);
    };

    // The state the defect is about: a freshly built skeleton has nothing to look at.
    TestNull(TEXT("a fresh skeleton starts with an empty preview slot"),
        static_cast<const USkeleton*>(Skeleton)->GetPreviewMesh());

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("skeletonPath"), Skeleton->GetPathName());
    Payload->SetStringField(TEXT("skeletalMeshPath"), Mesh->GetPathName());

    FTestResponseCapture Capture;
    if (!TestTrue(TEXT("skeleton.set_preview_mesh is registered"),
            InvokeHandlerWithCapture(TEXT("skeleton.set_preview_mesh"), Payload, Capture)))
    {
        return false;
    }
    if (!TestTrue(*FString::Printf(TEXT("the verb succeeded (errorCode='%s', message='%s')"),
            *Capture.ErrorCode, *Capture.Message), Capture.bSuccess))
    {
        return false;
    }

    TestEqual(TEXT("the response names the mesh it applied"),
        ReadString(Capture.Result, TEXT("previewMesh")), Mesh->GetPathName());
    TestTrue(TEXT("and says so as a measurement, not a literal"),
        ReadBool(Capture.Result, TEXT("previewMeshApplied"), false));
    TestTrue(TEXT("the slot changed"), ReadBool(Capture.Result, TEXT("changed"), false));
    TestEqual(TEXT("the previous occupant is reported so the change can be put back"),
        ReadString(Capture.Result, TEXT("previousPreviewMesh")), FString());

    // Persistence honesty. McpSafeAssetSave marks the package dirty and writes nothing, so a
    // saved:true here would be the exact defect AddMarkDirtySaveReport exists to prevent.
    TestTrue(TEXT("a write was requested"),
        ReadBool(Capture.Result, TEXT("saveRequested"), false));
    TestFalse(TEXT("but nothing reached disk: mark-dirty is not a save"),
        ReadBool(Capture.Result, TEXT("saved"), true));
    TestTrue(TEXT("and the caller is told a flush is still owed"),
        ReadBool(Capture.Result, TEXT("pendingFlush"), false));

    // Verify through a DIFFERENT verb rather than re-reading the handler's own response.
    {
        TSharedPtr<FJsonObject> InfoPayload = MakeShared<FJsonObject>();
        InfoPayload->SetStringField(TEXT("skeletonPath"), Skeleton->GetPathName());

        FTestResponseCapture InfoCapture;
        InvokeHandlerWithCapture(TEXT("skeleton.get_info"), InfoPayload, InfoCapture);
        TestTrue(TEXT("skeleton.get_info succeeded"), InfoCapture.bSuccess);
        TestEqual(TEXT("get_info reports the slot the other verb filled"),
            ReadString(InfoCapture.Result, TEXT("previewMesh")), Mesh->GetPathName());
    }

    // Re-setting the same mesh must not dirty the package again. Reporting changed:true here
    // would hand the caller an asset to save that needs no saving.
    {
        FTestResponseCapture Repeat;
        InvokeHandlerWithCapture(TEXT("skeleton.set_preview_mesh"), Payload, Repeat);
        TestTrue(TEXT("re-setting the same mesh still succeeds"), Repeat.bSuccess);
        TestFalse(TEXT("and reports no change"),
            ReadBool(Repeat.Result, TEXT("changed"), true));
        TestFalse(TEXT("so no write was requested either"),
            ReadBool(Repeat.Result, TEXT("saveRequested"), true));
        TestEqual(TEXT("the previous occupant is now the mesh itself"),
            ReadString(Repeat.Result, TEXT("previousPreviewMesh")), Mesh->GetPathName());
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSkeletonSetPreviewMeshRefusesMismatchTest,
    "PinWright.skeleton.set_preview_mesh.RefusesAMeshTheEngineWouldSilentlyClear",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSkeletonSetPreviewMeshRefusesMismatchTest::RunTest(const FString& Parameters)
{
    using namespace PwSkeletonPreviewMeshTests;

    const FString SkeletonPath = UniquePath(TEXT("SK_PreviewHost"));
    const FString OtherSkeletonPath = UniquePath(TEXT("SK_PreviewForeign"));
    const FString MeshPath = UniquePath(TEXT("SKM_PreviewForeign"));
    const FString UnboundMeshPath = UniquePath(TEXT("SKM_PreviewUnbound"));

    USkeleton* Skeleton = MakeSkeleton(SkeletonPath);
    USkeleton* OtherSkeleton = MakeSkeleton(OtherSkeletonPath);
    if (!TestNotNull(TEXT("probe skeleton created"), Skeleton) ||
        !TestNotNull(TEXT("foreign skeleton created"), OtherSkeleton))
    {
        return false;
    }

    USkeletalMesh* ForeignMesh = MakeMesh(MeshPath, OtherSkeleton);
    USkeletalMesh* UnboundMesh = MakeMesh(UnboundMeshPath, nullptr);
    if (!TestNotNull(TEXT("foreign mesh created"), ForeignMesh) ||
        !TestNotNull(TEXT("unbound mesh created"), UnboundMesh))
    {
        return false;
    }

    Skeleton->AddToRoot();
    OtherSkeleton->AddToRoot();
    ForeignMesh->AddToRoot();
    UnboundMesh->AddToRoot();
    ON_SCOPE_EXIT
    {
        UnboundMesh->RemoveFromRoot();
        ForeignMesh->RemoveFromRoot();
        OtherSkeleton->RemoveFromRoot();
        Skeleton->RemoveFromRoot();
        CleanupTestAsset(UnboundMeshPath);
        CleanupTestAsset(MeshPath);
        CleanupTestAsset(OtherSkeletonPath);
        CleanupTestAsset(SkeletonPath);
    };

    // A mesh with no skeleton at all can never survive the engine's fixup, whatever the host's
    // compatibility policy is, so this half always runs.
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("skeletonPath"), Skeleton->GetPathName());
        Payload->SetStringField(TEXT("skeletalMeshPath"), UnboundMesh->GetPathName());

        FTestResponseCapture Capture;
        InvokeHandlerWithCapture(TEXT("skeleton.set_preview_mesh"), Payload, Capture);
        TestFalse(TEXT("a mesh with no bound skeleton is refused, not stored"),
            Capture.bSuccess);
        TestEqual(TEXT("with SKELETON_MISMATCH"), Capture.ErrorCode,
            FString(TEXT("SKELETON_MISMATCH")));
        TestNull(TEXT("and the slot was left empty"),
            static_cast<const USkeleton*>(Skeleton)->GetPreviewMesh());
    }

    // The foreign-skeleton half depends on host policy: USkeleton::IsCompatibleForEditor
    // consults a global AreAllSkeletonsCompatibleDelegate that a project may bind to "always
    // true". Where that is bound, the mesh is genuinely compatible and refusing it would be
    // wrong -- so the assertions are stepped over and the run says so, rather than reporting a
    // red for a host policy or a green for assertions that never ran.
    if (Skeleton->IsCompatibleForEditor(OtherSkeleton))
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("host-declares-all-skeletons-compatible"),
            TEXT("USkeleton::IsCompatibleForEditor accepted an unrelated skeleton, so this host "
                 "cannot produce the mismatch this half asserts on."));
        return true;
    }

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("skeletonPath"), Skeleton->GetPathName());
    Payload->SetStringField(TEXT("skeletalMeshPath"), ForeignMesh->GetPathName());

    FTestResponseCapture Capture;
    InvokeHandlerWithCapture(TEXT("skeleton.set_preview_mesh"), Payload, Capture);

    TestFalse(TEXT("a mesh bound to an unrelated skeleton is refused, not stored"),
        Capture.bSuccess);
    TestEqual(TEXT("with SKELETON_MISMATCH"), Capture.ErrorCode,
        FString(TEXT("SKELETON_MISMATCH")));
    TestTrue(TEXT("and the refusal names the skeleton the mesh actually belongs to"),
        Capture.Message.Contains(OtherSkeleton->GetPathName()));

    // The point of refusing rather than warning: had it been stored, the next reader would
    // have cleared it and no one would have been told.
    TestNull(TEXT("the slot is still empty, so nothing was written to be erased later"),
        static_cast<const USkeleton*>(Skeleton)->GetPreviewMesh());

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSkeletonPathAndSocketTrapRegressionTest,
    "PinWright.skeleton.PathAndSocketTrapRegression",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSkeletonPathAndSocketTrapRegressionTest::RunTest(const FString& Parameters)
{
    using namespace PwSkeletonPreviewMeshTests;

    const FString SkeletonPath = UniquePath(TEXT("SK_TrapSkeleton"));
    const FString MeshPath = UniquePath(TEXT("SKM_TrapMesh"));
    USkeleton* Skeleton = MakeTwoBoneSkeleton(SkeletonPath);
    USkeletalMesh* Mesh = MakeMesh(MeshPath, Skeleton);
    if (!TestNotNull(TEXT("trap skeleton created"), Skeleton) ||
        !TestNotNull(TEXT("trap mesh created"), Mesh))
    {
        return false;
    }

    UMorphTarget* ExistingMorph = NewObject<UMorphTarget>(Mesh, FName(TEXT("ExistingMorph")), RF_Transactional);
    TArray<FMorphTargetDelta> ExistingDeltas;
    FMorphTargetDelta ExistingDelta;
    ExistingDelta.SourceIdx = 0;
    ExistingDelta.PositionDelta = FVector3f(1.0f, 0.0f, 0.0f);
    ExistingDeltas.Add(ExistingDelta);
    TArray<FSkelMeshSection> EmptySections;
    ExistingMorph->PopulateDeltas(ExistingDeltas, 0, EmptySections, false, false);
    Mesh->RegisterMorphTarget(ExistingMorph);
    if (!TestNotNull(TEXT("valid morph fixture created"), Mesh->FindMorphTarget(FName(TEXT("ExistingMorph")))))
    {
        return false;
    }

    Skeleton->AddToRoot();
    Mesh->AddToRoot();
    ON_SCOPE_EXIT
    {
        Mesh->RemoveFromRoot();
        Skeleton->RemoveFromRoot();
        CleanupTestAsset(MeshPath);
        CleanupTestAsset(SkeletonPath);
    };

    // The old implementation treated skeletonPath as a mesh path and either failed or edited
    // the mesh reference skeleton. The target of this verb is the USkeleton in both forms.
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("skeletonPath"), Skeleton->GetPathName());
        Payload->SetStringField(TEXT("boneName"), TEXT("root"));
        TSharedPtr<FJsonObject> Location = MakeShared<FJsonObject>();
        Location->SetNumberField(TEXT("x"), 12.0);
        Location->SetNumberField(TEXT("y"), 0.0);
        Location->SetNumberField(TEXT("z"), 0.0);
        Payload->SetObjectField(TEXT("location"), Location);

        FTestResponseCapture Capture;
        InvokeHandlerWithCapture(TEXT("skeleton.set_bone_transform"), Payload, Capture);
        TestTrue(TEXT("set_bone_transform accepts a skeletonPath"), Capture.bSuccess);
        TestEqual(TEXT("set_bone_transform reports the edited skeleton"),
            ReadString(Capture.Result, TEXT("skeletonPath")), Skeleton->GetPathName());
        TestTrue(TEXT("set_bone_transform changed the skeleton reference pose"),
            FMath::IsNearlyEqual(Skeleton->GetReferenceSkeleton().GetRefBonePose()[0].GetLocation().X, 12.0f));
    }

    // A real USkeleton passed in the mesh slot must not look like a missing mesh.
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("skeletalMeshPath"), Skeleton->GetPathName());
        Payload->SetStringField(TEXT("boneName"), TEXT("root"));
        FTestResponseCapture Capture;
        InvokeHandlerWithCapture(TEXT("skeleton.get_bone_transform"), Payload, Capture);
        TestFalse(TEXT("get_bone_transform rejects a skeleton in the mesh slot"), Capture.bSuccess);
        TestEqual(TEXT("wrong mesh type has a typed error"), Capture.ErrorCode,
            FString(TEXT("INVALID_ASSET_TYPE")));
        TestTrue(TEXT("wrong mesh type names the actual USkeleton"), Capture.Message.Contains(TEXT("USkeleton")));
        TestTrue(TEXT("wrong mesh type names the recovery"), Capture.Message.Contains(TEXT("USkeletalMesh")));
    }

    // The old skeleton-path fallback accepted a mesh and wrote a supposedly shared socket to its
    // bound Skeleton. That is a storage-target trap, not a useful fallback.
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("skeletonPath"), Mesh->GetPathName());
        Payload->SetStringField(TEXT("socketName"), TEXT("WrongTarget"));
        Payload->SetStringField(TEXT("attachBoneName"), TEXT("root"));
        FTestResponseCapture Capture;
        InvokeHandlerWithCapture(TEXT("skeleton.create_socket"), Payload, Capture);
        TestFalse(TEXT("create_socket rejects a mesh in skeletonPath"), Capture.bSuccess);
        TestEqual(TEXT("wrong socket target has a typed error"), Capture.ErrorCode,
            FString(TEXT("INVALID_ASSET_TYPE")));
        TestTrue(TEXT("wrong socket target names the recovery"), Capture.Message.Contains(TEXT("USkeleton")));
        TestEqual(TEXT("wrong socket target did not add a socket"), Skeleton->Sockets.Num(), 0);
    }

    // A socket anchored to a nonexistent bone reports a typed recovery and leaves no orphan
    // object behind.
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("skeletonPath"), Skeleton->GetPathName());
        Payload->SetStringField(TEXT("socketName"), TEXT("OrphanSocket"));
        Payload->SetStringField(TEXT("attachBoneName"), TEXT("missing_bone"));
        FTestResponseCapture Capture;
        InvokeHandlerWithCapture(TEXT("skeleton.create_socket"), Payload, Capture);
        TestFalse(TEXT("create_socket rejects a missing anchor bone"), Capture.bSuccess);
        TestEqual(TEXT("missing socket bone has a typed error"), Capture.ErrorCode,
            FString(TEXT("BONE_NOT_FOUND")));
        TestTrue(TEXT("missing socket bone names the recovery"), Capture.Message.Contains(TEXT("attachBoneName")));
        TestEqual(TEXT("missing socket bone did not create an orphan"), Skeleton->Sockets.Num(), 0);
    }

    // Create one valid socket for the configure/list/delete regression checks.
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("skeletonPath"), Skeleton->GetPathName());
        Payload->SetStringField(TEXT("socketName"), TEXT("Grip"));
        Payload->SetStringField(TEXT("attachBoneName"), TEXT("root"));
        FTestResponseCapture Capture;
        InvokeHandlerWithCapture(TEXT("skeleton.create_socket"), Payload, Capture);
        if (!TestTrue(TEXT("valid socket was created"), Capture.bSuccess))
        {
            return false;
        }
    }

    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("skeletonPath"), Skeleton->GetPathName());
        Payload->SetStringField(TEXT("socketName"), TEXT("Grip"));
        FTestResponseCapture Capture;
        InvokeHandlerWithCapture(TEXT("skeleton.configure_socket"), Payload, Capture);
        TestFalse(TEXT("configure_socket rejects a no-op request"), Capture.bSuccess);
        TestEqual(TEXT("configure_socket no-op has a missing update error"), Capture.ErrorCode,
            FString(TEXT("MISSING_PARAM")));
        TestTrue(TEXT("configure_socket no-op names the recovery"), Capture.Message.Contains(TEXT("relative")));
        TestEqual(TEXT("configure_socket no-op leaves the socket attached to root"),
            Skeleton->Sockets[0]->BoneName, FName(TEXT("root")));
    }

    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("skeletonPath"), Skeleton->GetPathName());
        Payload->SetStringField(TEXT("socketName"), TEXT("Grip"));
        Payload->SetStringField(TEXT("attachBoneName"), TEXT("missing_bone"));
        TSharedPtr<FJsonObject> Location = MakeShared<FJsonObject>();
        Location->SetNumberField(TEXT("x"), 1.0);
        Payload->SetObjectField(TEXT("relativeLocation"), Location);
        FTestResponseCapture Capture;
        InvokeHandlerWithCapture(TEXT("skeleton.configure_socket"), Payload, Capture);
        TestFalse(TEXT("configure_socket rejects a missing replacement bone"), Capture.bSuccess);
        TestEqual(TEXT("replacement bone has a typed error"), Capture.ErrorCode,
            FString(TEXT("BONE_NOT_FOUND")));
        TestTrue(TEXT("replacement bone names the recovery"), Capture.Message.Contains(TEXT("no change")));
        TestEqual(TEXT("rejected replacement leaves the socket unchanged"),
            Skeleton->Sockets[0]->BoneName, FName(TEXT("root")));
    }

    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("skeletonPath"), Skeleton->GetPathName());
        Payload->SetStringField(TEXT("skeletalMeshPath"), Mesh->GetPathName());
        FTestResponseCapture Capture;
        InvokeHandlerWithCapture(TEXT("skeleton.list_sockets"), Payload, Capture);
        TestFalse(TEXT("list_sockets rejects two storage targets"), Capture.bSuccess);
        TestEqual(TEXT("list_sockets reports the ambiguity"), Capture.ErrorCode,
            FString(TEXT("AMBIGUOUS_TARGET")));
    }

    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("skeletonPath"), Skeleton->GetPathName());
        Payload->SetStringField(TEXT("skeletalMeshPath"), Mesh->GetPathName());
        Payload->SetStringField(TEXT("socketName"), TEXT("Grip"));
        TSharedPtr<FJsonObject> Location = MakeShared<FJsonObject>();
        Location->SetNumberField(TEXT("x"), 2.0);
        Payload->SetObjectField(TEXT("relativeLocation"), Location);
        FTestResponseCapture Capture;
        InvokeHandlerWithCapture(TEXT("skeleton.configure_socket"), Payload, Capture);
        TestFalse(TEXT("configure_socket rejects two storage targets"), Capture.bSuccess);
        TestEqual(TEXT("configure_socket reports the ambiguity"), Capture.ErrorCode,
            FString(TEXT("AMBIGUOUS_TARGET")));
    }

    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("skeletonPath"), Skeleton->GetPathName());
        Payload->SetStringField(TEXT("skeletalMeshPath"), Mesh->GetPathName());
        Payload->SetStringField(TEXT("socketName"), TEXT("Grip"));
        FTestResponseCapture Capture;
        InvokeHandlerWithCapture(TEXT("skeleton.delete_socket"), Payload, Capture);
        TestFalse(TEXT("delete_socket rejects two storage targets"), Capture.bSuccess);
        TestEqual(TEXT("delete_socket reports the ambiguity"), Capture.ErrorCode,
            FString(TEXT("AMBIGUOUS_TARGET")));
        TestEqual(TEXT("ambiguous delete leaves the socket in place"), Skeleton->Sockets.Num(), 1);
    }

    {
        TArray<TSharedPtr<FJsonValue>> EmptyDeltas;
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("skeletalMeshPath"), Mesh->GetPathName());
        Payload->SetStringField(TEXT("morphTargetName"), TEXT("ExistingMorph"));
        Payload->SetArrayField(TEXT("deltas"), EmptyDeltas);
        FTestResponseCapture Capture;
        InvokeHandlerWithCapture(TEXT("skeleton.set_morph_target_deltas"), Payload, Capture);
        TestFalse(TEXT("set_morph_target_deltas rejects an empty update"), Capture.bSuccess);
        TestEqual(TEXT("empty morph update reports a missing delta"), Capture.ErrorCode,
            FString(TEXT("MISSING_PARAM")));
        TestTrue(TEXT("empty morph update names the recovery"), Capture.Message.Contains(TEXT("no morph data")));
    }

    // This direct socket-path check covers list_sockets' strict USkeleton loader as well.
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("skeletonPath"), Mesh->GetPathName());
        FTestResponseCapture Capture;
        InvokeHandlerWithCapture(TEXT("skeleton.list_sockets"), Payload, Capture);
        TestFalse(TEXT("list_sockets rejects a mesh in skeletonPath"), Capture.bSuccess);
        TestEqual(TEXT("list_sockets wrong class is typed"), Capture.ErrorCode,
            FString(TEXT("INVALID_ASSET_TYPE")));
        TestTrue(TEXT("list_sockets wrong class names the recovery"), Capture.Message.Contains(TEXT("USkeleton")));
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSkeletonSiblingPathTypeDiagnosticsTest,
    "PinWright.skeleton.SiblingPathTypeDiagnostics",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSkeletonSiblingPathTypeDiagnosticsTest::RunTest(const FString& Parameters)
{
    using namespace PwSkeletonPreviewMeshTests;

    const FString SkeletonPath = UniquePath(TEXT("SK_SiblingTrap"));
    const FString PhysicsPath = UniquePath(TEXT("PHYS_SiblingTrap"));
    USkeleton* Skeleton = MakeTwoBoneSkeleton(SkeletonPath);
    UPhysicsAsset* PhysicsAsset = MakePhysicsAsset(PhysicsPath);
    if (!TestNotNull(TEXT("sibling skeleton created"), Skeleton) ||
        !TestNotNull(TEXT("sibling physics asset created"), PhysicsAsset))
    {
        return false;
    }

    Skeleton->AddToRoot();
    PhysicsAsset->AddToRoot();
    ON_SCOPE_EXIT
    {
        PhysicsAsset->RemoveFromRoot();
        Skeleton->RemoveFromRoot();
        CleanupTestAsset(PhysicsPath);
        CleanupTestAsset(SkeletonPath);
    };

    auto AssertInvalidType = [this](const TCHAR* Verb, const TSharedPtr<FJsonObject>& Payload,
        const TCHAR* ActualType, const TCHAR* ExpectedType, const TCHAR* Label) -> bool
    {
        FTestResponseCapture Capture;
        if (!TestTrue(*FString::Printf(TEXT("%s was invoked"), Verb),
                InvokeHandlerWithCapture(Verb, Payload, Capture)))
        {
            return false;
        }
        if (!TestFalse(*FString::Printf(TEXT("%s rejected the wrong class"), Label), Capture.bSuccess))
        {
            return false;
        }
        if (!TestEqual(*FString::Printf(TEXT("%s returned INVALID_ASSET_TYPE"), Label), Capture.ErrorCode,
                FString(TEXT("INVALID_ASSET_TYPE"))))
        {
            return false;
        }
        TestTrue(*FString::Printf(TEXT("%s names the actual class"), Label), Capture.Message.Contains(ActualType));
        TestTrue(*FString::Printf(TEXT("%s names the expected class and recovery"), Label), Capture.Message.Contains(ExpectedType));
        return true;
    };

    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("skeletalMeshPath"), Skeleton->GetPathName());
        if (!AssertInvalidType(TEXT("skeleton.describe_mesh"), Payload, TEXT("USkeleton"), TEXT("USkeletalMesh"), TEXT("describe_mesh"))) return false;
    }
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("skeletalMeshPath"), Skeleton->GetPathName());
        if (!AssertInvalidType(TEXT("skeleton.describe_skin_weights"), Payload, TEXT("USkeleton"), TEXT("USkeletalMesh"), TEXT("describe_skin_weights"))) return false;
    }
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("skeletalMeshPath"), Skeleton->GetPathName());
        Payload->SetStringField(TEXT("profileName"), TEXT("TrapProfile"));
        if (!AssertInvalidType(TEXT("skeleton.normalize_weights"), Payload, TEXT("USkeleton"), TEXT("USkeletalMesh"), TEXT("normalize_weights"))) return false;
    }
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("skeletalMeshPath"), Skeleton->GetPathName());
        Payload->SetNumberField(TEXT("threshold"), 0.25);
        if (!AssertInvalidType(TEXT("skeleton.prune_weights"), Payload, TEXT("USkeleton"), TEXT("USkeletalMesh"), TEXT("prune_weights"))) return false;
    }
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("skeletalMeshPath"), Skeleton->GetPathName());
        if (!AssertInvalidType(TEXT("skeleton.auto_skin_weights"), Payload, TEXT("USkeleton"), TEXT("USkeletalMesh"), TEXT("auto_skin_weights"))) return false;
    }
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("skeletalMeshPath"), Skeleton->GetPathName());
        Payload->SetStringField(TEXT("morphTargetName"), TEXT("TrapMorph"));
        if (!AssertInvalidType(TEXT("skeleton.create_morph_target"), Payload, TEXT("USkeleton"), TEXT("USkeletalMesh"), TEXT("create_morph_target"))) return false;
    }
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("physicsAssetPath"), Skeleton->GetPathName());
        if (!AssertInvalidType(TEXT("skeleton.list_physics_bodies"), Payload, TEXT("USkeleton"), TEXT("UPhysicsAsset"), TEXT("list_physics_bodies"))) return false;
    }
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("physicsAssetPath"), Skeleton->GetPathName());
        if (!AssertInvalidType(TEXT("skeleton.get_physics_asset_info"), Payload, TEXT("USkeleton"), TEXT("UPhysicsAsset"), TEXT("get_physics_asset_info"))) return false;
    }
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("physicsAssetPath"), Skeleton->GetPathName());
        Payload->SetStringField(TEXT("boneName"), TEXT("root"));
        if (!AssertInvalidType(TEXT("skeleton.remove_physics_body"), Payload, TEXT("USkeleton"), TEXT("UPhysicsAsset"), TEXT("remove_physics_body"))) return false;
    }
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("physicsAssetPath"), Skeleton->GetPathName());
        Payload->SetStringField(TEXT("boneName"), TEXT("root"));
        Payload->SetNumberField(TEXT("mass"), 2.0);
        if (!AssertInvalidType(TEXT("skeleton.configure_physics_body"), Payload, TEXT("USkeleton"), TEXT("UPhysicsAsset"), TEXT("configure_physics_body"))) return false;
    }
    {
        TSharedPtr<FJsonObject> Limits = MakeShared<FJsonObject>();
        Limits->SetNumberField(TEXT("swing1LimitAngle"), 20.0);
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("physicsAssetPath"), Skeleton->GetPathName());
        Payload->SetStringField(TEXT("bodyA"), TEXT("root"));
        Payload->SetStringField(TEXT("bodyB"), TEXT("child"));
        Payload->SetObjectField(TEXT("limits"), Limits);
        if (!AssertInvalidType(TEXT("skeleton.configure_constraint_limits"), Payload, TEXT("USkeleton"), TEXT("UPhysicsAsset"), TEXT("configure_constraint_limits"))) return false;
    }
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("skeletalMeshPath"), Skeleton->GetPathName());
        Payload->SetStringField(TEXT("physicsAssetPath"), PhysicsAsset->GetPathName());
        if (!AssertInvalidType(TEXT("skeleton.set_physics_asset"), Payload, TEXT("USkeleton"), TEXT("USkeletalMesh"), TEXT("set_physics_asset"))) return false;
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSkeletonPhysicsConfigurationNoOpTest,
    "PinWright.skeleton.PhysicsConfigurationRejectsNoOp",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSkeletonPhysicsConfigurationNoOpTest::RunTest(const FString& Parameters)
{
    using namespace PwSkeletonPreviewMeshTests;

    const FString PhysicsPath = UniquePath(TEXT("PHYS_NoOp"));
    UPhysicsAsset* PhysicsAsset = MakePhysicsAsset(PhysicsPath);
    if (!TestNotNull(TEXT("no-op physics asset created"), PhysicsAsset))
    {
        return false;
    }
    PhysicsAsset->AddToRoot();
    ON_SCOPE_EXIT
    {
        PhysicsAsset->RemoveFromRoot();
        CleanupTestAsset(PhysicsPath);
    };

    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("physicsAssetPath"), PhysicsAsset->GetPathName());
        Payload->SetStringField(TEXT("boneName"), TEXT("root"));
        FTestResponseCapture Capture;
        InvokeHandlerWithCapture(TEXT("skeleton.configure_physics_body"), Payload, Capture);
        TestFalse(TEXT("configure_physics_body rejects a no-op"), Capture.bSuccess);
        TestEqual(TEXT("body no-op reports a missing setting"), Capture.ErrorCode,
            FString(TEXT("MISSING_PARAM")));
        TestTrue(TEXT("body no-op names the recovery"), Capture.Message.Contains(TEXT("mass")));
    }

    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("physicsAssetPath"), PhysicsAsset->GetPathName());
        Payload->SetStringField(TEXT("bodyA"), TEXT("root"));
        Payload->SetStringField(TEXT("bodyB"), TEXT("child"));
        FTestResponseCapture Capture;
        InvokeHandlerWithCapture(TEXT("skeleton.configure_constraint_limits"), Payload, Capture);
        TestFalse(TEXT("configure_constraint_limits rejects a no-op"), Capture.bSuccess);
        TestEqual(TEXT("constraint no-op reports a missing setting"), Capture.ErrorCode,
            FString(TEXT("MISSING_PARAM")));
        TestTrue(TEXT("constraint no-op names the recovery"), Capture.Message.Contains(TEXT("limits")));
    }

    return true;
}
