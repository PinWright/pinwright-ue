// Copyright (c) 2026 Alexander Penkin. MIT License.

// animation.get_live_pose against a placed /Engine/EngineMeshes/SkeletalCube actor in the editor
// world. The actor sits off the origin and is rotated, so world and component space differ and a
// space mix-up cannot pass; every bone in every space is compared to the engine's own
// GetSocketTransform for the same bone name.

#include "Misc/AutomationTest.h"

#include "Animation/SkeletalMeshActor.h"
#include "Components/SkeletalMeshComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/SkeletalMesh.h"
#include "Misc/Guid.h"
#include "ReferenceSkeleton.h"
#include "Tests/TestSkipReporting.h"
#include "Tests/TestUtils.h"
#include "Tests/TestWorldUtils.h"

namespace PwGetLivePoseTests
{
    const TCHAR* const Verb = TEXT("animation.get_live_pose");

    FVector ReadVector(const TSharedPtr<FJsonObject>& Object)
    {
        return FVector(Object->GetNumberField(TEXT("x")), Object->GetNumberField(TEXT("y")), Object->GetNumberField(TEXT("z")));
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGetLivePoseMatchesSocketTransformTest,
    "PinWright.animation.get_live_pose.BonesMatchSocketTransform",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGetLivePoseMatchesSocketTransformTest::RunTest(const FString& Parameters)
{
    using namespace PwGetLivePoseTests;

    FScopedEditorWorldActorGuard WorldGuard;
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    USkeletalMesh* Mesh = LoadObject<USkeletalMesh>(nullptr, TEXT("/Engine/EngineMeshes/SkeletalCube.SkeletalCube"));
    if (!World || !Mesh)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-unavailable"),
            TEXT("Skipped: no editor world or /Engine/EngineMeshes/SkeletalCube unavailable."));
        return true;
    }

    ASkeletalMeshActor* Actor = World->SpawnActor<ASkeletalMeshActor>(
        ASkeletalMeshActor::StaticClass(), FVector(120.0, -340.0, 55.0), FRotator(10.0, 35.0, -20.0));
    if (!TestNotNull(TEXT("probe actor spawned"), Actor))
    {
        return false;
    }
    const FString Label = FString::Printf(TEXT("PWLivePose_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    Actor->SetActorLabel(Label);
    USkeletalMeshComponent* Component = Actor->GetSkeletalMeshComponent();
    Component->SetSkeletalMeshAsset(Mesh);
    Component->RefreshBoneTransforms();

    const FReferenceSkeleton& RefSkeleton = Mesh->GetRefSkeleton();
    if (!TestEqual(TEXT("pose buffer populated"), Component->GetComponentSpaceTransforms().Num(), RefSkeleton.GetNum()))
    {
        return false;
    }

    UPackage* LevelPackage = World->PersistentLevel->GetOutermost();
    const bool bLevelDirty = LevelPackage->IsDirty();

    const TPair<const TCHAR*, ERelativeTransformSpace> Spaces[] = {
        {TEXT("world"), RTS_World}, {TEXT("component"), RTS_Component}, {TEXT("local"), RTS_ParentBoneSpace}};
    for (const TPair<const TCHAR*, ERelativeTransformSpace>& Space : Spaces)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("actorName"), Label);
        Payload->SetStringField(TEXT("world"), TEXT("editor"));
        Payload->SetStringField(TEXT("space"), Space.Key);
        TArray<TSharedPtr<FJsonValue>> Include;
        Include.Add(MakeShared<FJsonValueString>(TEXT("montage")));
        Payload->SetArrayField(TEXT("include"), Include);
        FTestResponseCapture Capture;
        TestTrue(TEXT("handler registered"), InvokeHandlerWithCapture(Verb, Payload, Capture));
        if (!TestTrue(FString::Printf(TEXT("%s: call succeeds (%s %s)"), Space.Key, *Capture.ErrorCode, *Capture.Message),
                Capture.bSuccess) || !Capture.Result.IsValid())
        {
            return false;
        }

        const TArray<TSharedPtr<FJsonValue>>& Bones = Capture.Result->GetArrayField(TEXT("bones"));
        TestEqual(FString::Printf(TEXT("%s: every bone returned"), Space.Key), Bones.Num(), RefSkeleton.GetNum());
        for (const TSharedPtr<FJsonValue>& Value : Bones)
        {
            const TSharedPtr<FJsonObject> Bone = Value->AsObject();
            const FName BoneName(*Bone->GetStringField(TEXT("name")));
            const FTransform Expected = Component->GetSocketTransform(BoneName, Space.Value);
            const TSharedPtr<FJsonObject> Transform = Bone->GetObjectField(TEXT("transform"));
            const FVector Location = ReadVector(Transform->GetObjectField(TEXT("location")));
            const TSharedPtr<FJsonObject> RotationJson = Transform->GetObjectField(TEXT("rotation"));
            const FRotator Rotation(RotationJson->GetNumberField(TEXT("pitch")),
                RotationJson->GetNumberField(TEXT("yaw")), RotationJson->GetNumberField(TEXT("roll")));
            TestTrue(FString::Printf(TEXT("%s %s location matches GetSocketTransform"), Space.Key, *BoneName.ToString()),
                Location.Equals(Expected.GetLocation(), 1e-3));
            TestTrue(FString::Printf(TEXT("%s %s rotation matches GetSocketTransform"), Space.Key, *BoneName.ToString()),
                Rotation.Quaternion().AngularDistance(Expected.GetRotation()) < 1e-3);
        }

        const TSharedPtr<FJsonObject> PoseBuffer = Capture.Result->GetObjectField(TEXT("poseBuffer"));
        TestTrue(TEXT("poseBuffer.revision is the component's revision"),
            static_cast<uint32>(PoseBuffer->GetNumberField(TEXT("revision"))) == Component->GetBoneTransformRevisionNumber());
        TestTrue(TEXT("poseBuffer.frameCounter is GFrameCounter"),
            static_cast<uint64>(PoseBuffer->GetNumberField(TEXT("frameCounter"))) == static_cast<uint64>(GFrameCounter));
        TestEqual(TEXT("poseBuffer.tickedThisFrame mirrors the component"),
            PoseBuffer->GetBoolField(TEXT("tickedThisFrame")), Component->PoseTickedThisFrame());
        TestTrue(TEXT("montage is null without an active montage"),
            Capture.Result->HasTypedField<EJson::Null>(TEXT("montage")));
    }

    TestEqual(TEXT("level package dirty flag unchanged by the read"), LevelPackage->IsDirty(), bLevelDirty);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGetLivePosePieWithoutSessionTest,
    "PinWright.animation.get_live_pose.PieWorldWithoutSessionRefused",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGetLivePosePieWithoutSessionTest::RunTest(const FString& Parameters)
{
    if (GEditor && GEditor->PlayWorld)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("pie-active"),
            TEXT("Skipped: a PIE session is running, so world:'pie' resolves."));
        return true;
    }

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("actorName"), TEXT("AnyActor"));
    Payload->SetStringField(TEXT("world"), TEXT("pie"));
    Payload->SetStringField(TEXT("space"), TEXT("world"));
    FTestResponseCapture Capture;
    TestTrue(TEXT("handler registered"), InvokeHandlerWithCapture(PwGetLivePoseTests::Verb, Payload, Capture));
    TestFalse(TEXT("no PIE session is an error"), Capture.bSuccess);
    TestEqual(TEXT("with PIE_NOT_ACTIVE"), Capture.ErrorCode, FString(TEXT("PIE_NOT_ACTIVE")));
    return true;
}
