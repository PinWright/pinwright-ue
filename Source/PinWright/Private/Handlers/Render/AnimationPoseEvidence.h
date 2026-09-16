// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Components/SkinnedMeshComponent.h"
#include "Dom/JsonObject.h"

// Numeric proof that a skinned mesh's POSE actually changed between the frames of a capture
// burst — the one thing a set of images cannot establish on its own.
//
// A mesh frozen in bind pose photographs identically at every frame and from every angle, so a
// capture set cannot distinguish "the animation is subtle" from "nothing is animating". That is
// not hypothetical: a shipped layer of skinned actors was reviewed from stills and signed off
// while every mesh evaluated in bind pose, sliding across the level on its actor transform
// alone, because the sequence carried transform keys but no skeletal animation track.
// Sampling the component-space bone transforms at each burst frame settles it in numbers,
// before a single pixel is read.
//
// Component space is deliberate. It measures the POSE and nothing else: an actor translating
// across the level does not move a single component-space bone, so pose change and actor motion
// are independent measurements. Reporting both is what separates "sliding in bind pose" from
// "animating on the spot" from "not moving at all".
namespace PinWrightAnimationPose
{
    // Bone motion below this is measurement noise, not animation. 0.01 cm is a tenth of a
    // millimetre at Unreal's centimetre scale — far under anything a viewer could see, and far
    // over float drift in a skinning evaluation.
    constexpr double BoneMoveThresholdCm = 0.01;

    // Rotation-only motion of a leaf bone moves no location at all, so a translation-only test
    // would call a wrist twist "no movement". A tenth of a degree is the matching noise floor.
    constexpr double BoneRotateThresholdDegrees = 0.1;

    struct FPoseSample
    {
        bool bValid = false;
        // Component-space bone transforms, copied at the instant the sample was taken. The
        // component's own array is double-buffered and is overwritten by the next evaluation,
        // so a reference would silently compare a frame against itself.
        TArray<FTransform> BoneTransforms;
        FTransform ComponentToWorld = FTransform::Identity;
    };

    struct FPoseDelta
    {
        bool bComparable = false;
        double MaxTranslationCm = 0.0;
        double MaxRotationDegrees = 0.0;
        int32 MovedBoneCount = 0;
        // World-space distance the component itself travelled between the two samples.
        double ComponentTranslationCm = 0.0;
    };

    // Copy the component's current component-space pose. Returns false when the component has
    // never been evaluated (empty transform array), which is itself a finding: it means the
    // animation system never ran for this component.
    inline bool SamplePose(const USkinnedMeshComponent* Component, FPoseSample& OutSample)
    {
        OutSample = FPoseSample();
        if (!Component)
        {
            return false;
        }
        const TArray<FTransform>& Transforms = Component->GetComponentSpaceTransforms();
        if (Transforms.Num() == 0)
        {
            return false;
        }
        OutSample.BoneTransforms = Transforms;
        OutSample.ComponentToWorld = Component->GetComponentTransform();
        OutSample.bValid = true;
        return true;
    }

    inline FPoseDelta ComparePose(const FPoseSample& A, const FPoseSample& B)
    {
        FPoseDelta Delta;
        if (!A.bValid || !B.bValid)
        {
            return Delta;
        }
        Delta.ComponentTranslationCm = FVector::Dist(
            A.ComponentToWorld.GetLocation(), B.ComponentToWorld.GetLocation());

        // A changed bone count means the mesh or its LOD swapped mid-burst; the poses are not
        // comparable and saying so is more useful than comparing the overlapping prefix and
        // reporting a number that means nothing.
        if (A.BoneTransforms.Num() != B.BoneTransforms.Num())
        {
            return Delta;
        }
        Delta.bComparable = true;

        for (int32 Index = 0; Index < A.BoneTransforms.Num(); ++Index)
        {
            const double Translation = FVector::Dist(
                A.BoneTransforms[Index].GetLocation(), B.BoneTransforms[Index].GetLocation());
            const double Rotation = FMath::RadiansToDegrees(
                A.BoneTransforms[Index].GetRotation().AngularDistance(B.BoneTransforms[Index].GetRotation()));

            Delta.MaxTranslationCm = FMath::Max(Delta.MaxTranslationCm, Translation);
            Delta.MaxRotationDegrees = FMath::Max(Delta.MaxRotationDegrees, Rotation);
            if (Translation > BoneMoveThresholdCm || Rotation > BoneRotateThresholdDegrees)
            {
                ++Delta.MovedBoneCount;
            }
        }
        return Delta;
    }

    inline bool PoseChanged(const FPoseDelta& Delta)
    {
        return Delta.bComparable && Delta.MovedBoneCount > 0;
    }

    inline void AddPoseDeltaFields(const FPoseDelta& Delta, TSharedPtr<FJsonObject>& Out)
    {
        Out->SetBoolField(TEXT("comparable"), Delta.bComparable);
        Out->SetNumberField(TEXT("maxBoneTranslationCm"), Delta.MaxTranslationCm);
        Out->SetNumberField(TEXT("maxBoneRotationDegrees"), Delta.MaxRotationDegrees);
        Out->SetNumberField(TEXT("movedBoneCount"), Delta.MovedBoneCount);
        Out->SetNumberField(TEXT("componentTranslationCm"), Delta.ComponentTranslationCm);
    }
}
