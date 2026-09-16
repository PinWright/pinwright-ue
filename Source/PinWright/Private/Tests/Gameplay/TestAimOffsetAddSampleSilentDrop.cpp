// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for B-blend-space-add-sample-silent-drop:
// animation.authoring.add_aim_offset_sample discards the int32 return of
// UBlendSpace::AddSample (AnimationAuthoringHandler_BlendSpace.cpp:730) and reports
// success unconditionally. A UAimOffsetBlendSpace accepts ONLY AAT_RotationOffsetMeshSpace
// additive clips (AimOffsetBlendSpace.cpp: IsValidAdditiveType), so a normal non-additive
// (AAT_None) clip is rejected 100% of the time: AddSample returns INDEX_NONE and the
// SampleData array stays empty — yet the handler still answers "Aim offset sample added".
//
// This test builds a matched skeleton + a non-additive UAnimSequence + a
// UAimOffsetBlendSpace in-code, invokes the real registered handler, confirms the engine
// genuinely rejected the sample (GetNumberOfBlendSamples stays 0), and asserts the handler
// must NOT report success when nothing was added. Pre-fix the success report is
// unconditional so the bSuccess==false assertion fails (RED); a fix that captures the
// INDEX_NONE return and errors flips it green.
#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Animation/AnimSequence.h"
#include "Animation/AnimationAsset.h"
#include "Animation/BlendSpace.h"
#include "Animation/AimOffsetBlendSpace.h"
#include "Animation/Skeleton.h"
#include "ReferenceSkeleton.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/HandlerRegistration.h"
#include "Tests/TestUtils.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#include "Misc/Guid.h"

// Uniquely-named namespace (not anonymous) so the in-code fixture builders never
// ODR-collide with same-named file-static helpers in sibling animation tests when
// the unity build merges translation units.
namespace PinWrightAimOffsetSilentDropTest
{
    // Rooted, path-loadable USkeleton with a single "root" bone. Mirrors the
    // /Game/PinWrightTests package + RF_Transient + AddToRoot + object-path convention
    // the sibling animation tests use so the handler's StaticLoadObject finds it in memory.
    static USkeleton* MakeRootedSkeleton(FString& OutObjectPath)
    {
        const FString AssetName = FString::Printf(TEXT("SK_AOSilentDrop_%s"),
            *FGuid::NewGuid().ToString(EGuidFormats::Digits));
        const FString PackageName = FString::Printf(TEXT("/Game/PinWrightTests/%s"), *AssetName);
        UPackage* Package = CreatePackage(*PackageName);
        USkeleton* Skeleton = NewObject<USkeleton>(Package, *AssetName, RF_Public | RF_Standalone | RF_Transient);
        if (!Skeleton)
        {
            return nullptr;
        }
        {
            FReferenceSkeletonModifier Modifier(Skeleton);
            FMeshBoneInfo RootBone;
            RootBone.Name = FName(TEXT("root"));
            RootBone.ParentIndex = INDEX_NONE;
            RootBone.ExportName = TEXT("root");
            Modifier.Add(RootBone, FTransform::Identity, true);
        }
        Skeleton->AddToRoot();
        OutObjectPath = FString::Printf(TEXT("%s.%s"), *PackageName, *AssetName);
        return Skeleton;
    }

    // Rooted, path-loadable non-additive UAnimSequence on the given skeleton. A fresh
    // UAnimSequence defaults to AdditiveAnimType == AAT_None — exactly the clip an
    // AimOffset rejects — so no additive setup is performed (and must not be).
    static UAnimSequence* MakeRootedNonAdditiveSequence(USkeleton* Skeleton, FString& OutObjectPath)
    {
        const FString AssetName = FString::Printf(TEXT("AS_AOSilentDrop_%s"),
            *FGuid::NewGuid().ToString(EGuidFormats::Digits));
        const FString PackageName = FString::Printf(TEXT("/Game/PinWrightTests/%s"), *AssetName);
        UPackage* Package = CreatePackage(*PackageName);
        UAnimSequence* Sequence = NewObject<UAnimSequence>(Package, *AssetName, RF_Public | RF_Standalone | RF_Transient);
        if (!Sequence)
        {
            return nullptr;
        }
        Sequence->SetSkeleton(Skeleton);
        Sequence->AddToRoot();
        OutObjectPath = FString::Printf(TEXT("%s.%s"), *PackageName, *AssetName);
        return Sequence;
    }

    // Rooted, path-loadable UAimOffsetBlendSpace on the given skeleton. Default
    // FBlendParameter axes (Min 0 / Max 100 / GridNum 4 per axis) make (0,0) an in-range
    // sample, so the ONLY rejection cause is the non-additive clip.
    static UAimOffsetBlendSpace* MakeRootedAimOffset(USkeleton* Skeleton, FString& OutObjectPath)
    {
        const FString AssetName = FString::Printf(TEXT("AO_AOSilentDrop_%s"),
            *FGuid::NewGuid().ToString(EGuidFormats::Digits));
        const FString PackageName = FString::Printf(TEXT("/Game/PinWrightTests/%s"), *AssetName);
        UPackage* Package = CreatePackage(*PackageName);
        UAimOffsetBlendSpace* AimOffset = NewObject<UAimOffsetBlendSpace>(
            Package, *AssetName, RF_Public | RF_Standalone | RF_Transient);
        if (!AimOffset)
        {
            return nullptr;
        }
        AimOffset->SetSkeleton(Skeleton);
        AimOffset->AddToRoot();
        OutObjectPath = FString::Printf(TEXT("%s.%s"), *PackageName, *AssetName);
        return AimOffset;
    }

    // Rooted, path-loadable plain UBlendSpace (2D) on the given skeleton. Default FBlendParameter
    // axes (Min 0 / Max 100 per axis) put (0,0) in range, so a first add at (0,0) is accepted and
    // a second add at the SAME (0,0) is rejected as a duplicate sample point
    // (UBlendSpace::IsTooCloseToExistingSamplePoint) — the deterministic add_blend_sample reject
    // (a plain blend space accepts AAT_None, so an additive-type mismatch can't be the trigger).
    static UBlendSpace* MakeRootedBlendSpace(USkeleton* Skeleton, FString& OutObjectPath)
    {
        const FString AssetName = FString::Printf(TEXT("BS_AOSilentDrop_%s"),
            *FGuid::NewGuid().ToString(EGuidFormats::Digits));
        const FString PackageName = FString::Printf(TEXT("/Game/PinWrightTests/%s"), *AssetName);
        UPackage* Package = CreatePackage(*PackageName);
        UBlendSpace* BlendSpace = NewObject<UBlendSpace>(
            Package, *AssetName, RF_Public | RF_Standalone | RF_Transient);
        if (!BlendSpace)
        {
            return nullptr;
        }
        BlendSpace->SetSkeleton(Skeleton);
        BlendSpace->AddToRoot();
        OutObjectPath = FString::Printf(TEXT("%s.%s"), *PackageName, *AssetName);
        return BlendSpace;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAimOffsetAddSampleRejectedNotFakeSuccessTest,
    "PinWright.animation.authoring.add_aim_offset_sample.RejectedSampleReportsFailure",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FAimOffsetAddSampleRejectedNotFakeSuccessTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightAimOffsetSilentDropTest;

    FString SkeletonPath;
    USkeleton* Skeleton = MakeRootedSkeleton(SkeletonPath);
    TestNotNull(TEXT("transient skeleton created"), Skeleton);
    if (!Skeleton)
    {
        return false;
    }

    FString SequencePath;
    UAnimSequence* Sequence = MakeRootedNonAdditiveSequence(Skeleton, SequencePath);
    TestNotNull(TEXT("transient non-additive sequence created"), Sequence);
    if (!Sequence)
    {
        Skeleton->RemoveFromRoot();
        return false;
    }

    // Ground-truth precondition: the fixture clip is non-additive, which is exactly what
    // a UAimOffsetBlendSpace rejects (IsValidAdditiveType accepts only AAT_RotationOffsetMeshSpace).
    TestEqual(TEXT("fixture clip is AAT_None (non-additive)"),
        static_cast<int32>(Sequence->AdditiveAnimType), static_cast<int32>(AAT_None));

    FString AimOffsetPath;
    UAimOffsetBlendSpace* AimOffset = MakeRootedAimOffset(Skeleton, AimOffsetPath);
    TestNotNull(TEXT("transient aim offset created"), AimOffset);
    if (!AimOffset)
    {
        Sequence->RemoveFromRoot();
        Skeleton->RemoveFromRoot();
        return false;
    }

    const int32 NumBefore = AimOffset->GetNumberOfBlendSamples();

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), AimOffsetPath);
    Payload->SetStringField(TEXT("animationPath"), SequencePath);
    // (0,0) is deliberately in-range for the default axes so the SOLE rejection cause is
    // the non-additive clip — matching the ticket's verbatim repro (yaw:0, pitch:0).
    Payload->SetNumberField(TEXT("yaw"), 0.0);
    Payload->SetNumberField(TEXT("pitch"), 0.0);
    Payload->SetBoolField(TEXT("save"), false);

    FTestResponseCapture Capture;
    const bool bFound = InvokeHandlerWithCapture(
        TEXT("animation.authoring.add_aim_offset_sample"), Payload, Capture);
    TestTrue(TEXT("animation.authoring.add_aim_offset_sample handler found"), bFound);
    TestTrue(TEXT("handler responded"), Capture.bWasCalled);

    // The engine rejected the non-additive clip: AddSample returned INDEX_NONE and the
    // SampleData array did not grow. This holds regardless of the handler bug and proves
    // the reject path is genuinely exercised by the fixture.
    const int32 NumAfter = AimOffset->GetNumberOfBlendSamples();
    TestEqual(TEXT("engine rejected the sample: blend-sample count unchanged"), NumAfter, NumBefore);

    // THE DEFECT: the handler discards AddSample's INDEX_NONE return and reports success
    // anyway. Correct behavior is to report failure when no sample was added. Pre-fix this
    // is a fake success (Capture.bSuccess == true) so this assertion FAILS (RED); a fix that
    // treats INDEX_NONE as an error flips Capture.bSuccess false (GREEN).
    TestFalse(TEXT("add_aim_offset_sample must NOT report success when the sample was rejected"),
        Capture.bSuccess);
    // The fix reports the rejection with the SAMPLE_REJECTED error code (pre-fix ErrorCode is
    // empty because SendSuccess sets none), so this asserts the exact new error path is taken.
    TestEqual(TEXT("add_aim_offset_sample rejection uses the SAMPLE_REJECTED error code"),
        Capture.ErrorCode, FString(TEXT("SAMPLE_REJECTED")));

    AimOffset->RemoveFromRoot();
    Sequence->RemoveFromRoot();
    Skeleton->RemoveFromRoot();
    return true;
}

// Sibling of the aim-offset case for the SAME defect on the plain-blend-space verb:
// animation.authoring.add_blend_sample (AnimationAuthoringHandler_BlendSpace.cpp:500) discards the
// int32 return of UBlendSpace::AddSample and reports success unconditionally. A plain blend space
// accepts a non-additive clip, so the deterministic reject here is a DUPLICATE coordinate: the
// first add at (0,0) succeeds, the second add at (0,0) is rejected (IsTooCloseToExistingSamplePoint)
// and AddSample returns INDEX_NONE while the handler still answers success (RED pre-fix / GREEN once
// the fix errors with SAMPLE_REJECTED). This also guards that the fix leaves the valid first add
// reporting success (the fix must not break the happy path).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlendSpaceAddSampleRejectedNotFakeSuccessTest,
    "PinWright.animation.authoring.add_blend_sample.RejectedSampleReportsFailure",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FBlendSpaceAddSampleRejectedNotFakeSuccessTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightAimOffsetSilentDropTest;

    FString SkeletonPath;
    USkeleton* Skeleton = MakeRootedSkeleton(SkeletonPath);
    TestNotNull(TEXT("transient skeleton created"), Skeleton);
    if (!Skeleton)
    {
        return false;
    }

    FString SequencePath;
    UAnimSequence* Sequence = MakeRootedNonAdditiveSequence(Skeleton, SequencePath);
    TestNotNull(TEXT("transient sequence created"), Sequence);
    if (!Sequence)
    {
        Skeleton->RemoveFromRoot();
        return false;
    }

    FString BlendSpacePath;
    UBlendSpace* BlendSpace = MakeRootedBlendSpace(Skeleton, BlendSpacePath);
    TestNotNull(TEXT("transient blend space created"), BlendSpace);
    if (!BlendSpace)
    {
        Sequence->RemoveFromRoot();
        Skeleton->RemoveFromRoot();
        return false;
    }

    // Drives animation.authoring.add_blend_sample at (X,Y) on the fixture; returns whether the
    // handler was found. The 2D {x,y} object form matches the handler's sampleValue parsing.
    auto AddSampleAt = [&](double X, double Y, FTestResponseCapture& Capture) -> bool
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), BlendSpacePath);
        Payload->SetStringField(TEXT("animationPath"), SequencePath);
        TSharedPtr<FJsonObject> SampleObj = MakeShared<FJsonObject>();
        SampleObj->SetNumberField(TEXT("x"), X);
        SampleObj->SetNumberField(TEXT("y"), Y);
        Payload->SetObjectField(TEXT("sampleValue"), SampleObj);
        Payload->SetBoolField(TEXT("save"), false);
        return InvokeHandlerWithCapture(TEXT("animation.authoring.add_blend_sample"), Payload, Capture);
    };

    // First add at (0,0): a matched-skeleton AAT_None clip at an in-range coordinate is accepted
    // by a plain blend space. The fix must NOT turn this valid add into an error (happy-path guard).
    FTestResponseCapture FirstCapture;
    const bool bFound = AddSampleAt(0.0, 0.0, FirstCapture);
    TestTrue(TEXT("animation.authoring.add_blend_sample handler found"), bFound);
    TestTrue(TEXT("first (valid) add reports success"), FirstCapture.bSuccess);
    const int32 NumAfterFirst = BlendSpace->GetNumberOfBlendSamples();
    TestEqual(TEXT("engine accepted the first sample (count is 1)"), NumAfterFirst, 1);

    // Second add at the SAME (0,0): the engine rejects it as a duplicate sample point, so AddSample
    // returns INDEX_NONE and the sample count does not grow. Pre-fix the handler discards that
    // return and still reports success (RED); the fix reports SAMPLE_REJECTED instead (GREEN).
    FTestResponseCapture SecondCapture;
    AddSampleAt(0.0, 0.0, SecondCapture);
    TestTrue(TEXT("handler responded to the duplicate add"), SecondCapture.bWasCalled);
    const int32 NumAfterSecond = BlendSpace->GetNumberOfBlendSamples();
    TestEqual(TEXT("engine rejected the duplicate: blend-sample count unchanged"), NumAfterSecond, 1);
    TestFalse(TEXT("add_blend_sample must NOT report success when the sample was rejected"),
        SecondCapture.bSuccess);
    TestEqual(TEXT("add_blend_sample rejection uses the SAMPLE_REJECTED error code"),
        SecondCapture.ErrorCode, FString(TEXT("SAMPLE_REJECTED")));

    BlendSpace->RemoveFromRoot();
    Sequence->RemoveFromRoot();
    Skeleton->RemoveFromRoot();
    return true;
}
