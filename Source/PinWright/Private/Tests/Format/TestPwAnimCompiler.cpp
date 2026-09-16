// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"

#include "Compat/EngineVersionCompat.h"
#include "PwAnim/PwAnimCompiler.h"
#include "PwAnim/PwAnimDiagnostic.h"
#include "Tests/Assets/AnimAuthoringTestFixtures.h"
#include "Tests/TestSkipReporting.h"

#include "Animation/AnimData/IAnimationDataModel.h"
#include "Animation/AnimSequence.h"
#include "Animation/Skeleton.h"
#include "UObject/UObjectGlobals.h"

namespace
{
    const FPwDiagnostic* PwAnimCompilerTest_FindCode(
        const TArray<FPwDiagnostic>& Diagnostics, const TCHAR* Code)
    {
        for (const FPwDiagnostic& Diagnostic : Diagnostics)
        {
            if (Diagnostic.Code == Code)
            {
                return &Diagnostic;
            }
        }
        return nullptr;
    }

    FString PwAnimCompilerTest_Source(const FString& SkeletonPath, const TCHAR* BoneBody,
                                      int32 NumberOfFrames, bool bLoop)
    {
        return FString::Printf(
            TEXT("pwanim 0\n")
            TEXT("use skeleton from \"%s\"\n")
            TEXT("timebase rate=(30, 1) frames=%d loop=%s\n")
            TEXT("%s"),
            *SkeletonPath, NumberOfFrames, bLoop ? TEXT("true") : TEXT("false"), BoneBody);
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwAnimCompilerDenseBakeTest,
    "PinWright.Animation.Compiler.BakesDenseReferencePoseTracks",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwAnimCompilerDenseBakeTest::RunTest(const FString& Parameters)
{
#if UE_VERSION_OLDER_THAN(5, 5, 0)
    // Closing the controller bracket inside the .pwanim asset path can trigger the engine's
    // synchronous anim-compression DDC assert on UE 5.3/5.4.  This gate covers the whole
    // compiler call, not only the direct AnimSequenceCreate seam.
    PinWrightTestSkip::SkipAssertions(*this, TEXT("engine-version-unsupported"),
        TEXT("Skipping BakesDenseReferencePoseTracks on UE 5.3/5.4: engine anim "
            "compression asserts while closing the generated AnimSequence authoring bracket."));
    return true;
#else
    using namespace AnimAuthoringTestFixtures;

    const FName PelvisBone(TEXT("pelvis"));
    const FName SpineBone(TEXT("spine_01"));
    const TArray<FName> BoneNames = { PelvisBone, SpineBone };
    const TArray<FTransform> RefPoses = {
        FTransform::Identity,
        FTransform(FRotator(0.0f, 25.0f, 0.0f), FVector(10.0f, 20.0f, 30.0f),
                   FVector(1.5f, 2.0f, 0.5f))
    };

    USkeleton* Skeleton = NewTransientSkeletonWithBones(BoneNames, RefPoses);
    FScopedAnimAssetRoot SkeletonRoot(Skeleton);
    TestNotNull(TEXT("Transient skeleton with non-identity reference pose created"), Skeleton);
    if (!Skeleton)
    {
        return false;
    }
    const FString OutputAssetPath = FString::Printf(
        TEXT("/Game/PinWrightTests/%s"),
        *MakeUniqueAnimSeqTestAssetName(TEXT("AS_PwAnimCompiler")));
    const FString Source = PwAnimCompilerTest_Source(
        Skeleton->GetPathName(),
        TEXT("bone \"pelvis\" {\n")
        TEXT("    key frame=0 at=(0, 0, 0) ease=ease_in\n")
        TEXT("    key frame=4 at=(0, 0, 4)\n")
        TEXT("}\n")
        TEXT("bone \"spine_01\" {\n")
        TEXT("    key frame=0 rotate=(0, 0, 350)\n")
        TEXT("    key frame=4 rotate=(0, 0, 10)\n")
        TEXT("}\n"),
        4, false);

    FPwAnimCompileOptions Options;
    Options.OutputAssetPath = OutputAssetPath;
    Options.bSave = false;
    const FPwAnimCompileResult Result = FPwAnimCompiler::Compile(Source, Options);

    TestTrue(*FString::Printf(TEXT(".pwanim compile succeeds. [%s]"),
        *JoinPwDiagnostics(Result.Diagnostics)), Result.bSuccess);
    if (!Result.bSuccess)
    {
        return false;
    }

    TestEqual(TEXT("resolved skeleton path is measured from the asset"),
        Result.SkeletonPath, Skeleton->GetPathName());
    TestEqual(TEXT("requested frame count"), Result.RequestedNumberOfFrames, 4);
    TestEqual(TEXT("requested dense key count"), Result.RequestedNumberOfKeys, 5);
    TestEqual(TEXT("readback frame count"), Result.AssetNumberOfFrames, 4);
    TestEqual(TEXT("readback dense key count"), Result.AssetNumberOfKeys, 5);
    TestEqual(TEXT("readback track count"), Result.AssetTrackCount, 2);
    for (const int32 KeyCount : Result.AssetKeyCountPerTrack)
    {
        TestEqual(TEXT("each authored track has frames plus one keys"), KeyCount, 5);
    }

    UAnimSequence* Sequence = LoadObject<UAnimSequence>(nullptr, *Result.AssetPath);
    TestNotNull(TEXT("generated AnimSequence can be loaded for value readback"), Sequence);
    if (!Sequence)
    {
        return false;
    }
    Sequence->AddToRoot();
    ON_SCOPE_EXIT { Sequence->RemoveFromRoot(); };

    const IAnimationDataModel* Model = Sequence->GetDataModel();
    TestNotNull(TEXT("generated AnimSequence has a data model"), Model);
    if (!Model)
    {
        return false;
    }

    TArray<FFrameNumber> Frames;
    for (int32 Frame = 0; Frame <= 4; ++Frame)
    {
        Frames.Add(FFrameNumber(Frame));
    }

    TArray<FTransform> PelvisTransforms;
    Model->GetBoneTrackTransforms(PelvisBone, Frames, PelvisTransforms);
    TestEqual(TEXT("pelvis readback has one transform per dense sample"), PelvisTransforms.Num(), 5);
    if (PelvisTransforms.Num() == 5)
    {
        TestTrue(TEXT("ease_in changes the midpoint before linear halfway"),
            FMath::IsNearlyEqual(PelvisTransforms[2].GetTranslation().Z, 1.0f, 0.01f));
    }

    TArray<FTransform> SpineTransforms;
    Model->GetBoneTrackTransforms(SpineBone, Frames, SpineTransforms);
    TestEqual(TEXT("spine readback has one transform per dense sample"), SpineTransforms.Num(), 5);
    if (SpineTransforms.Num() == 5)
    {
        TestTrue(TEXT("unkeyed spine translation uses the reference pose"),
            SpineTransforms[2].GetTranslation().Equals(FVector(10.0f, 20.0f, 30.0f), 0.01f));
        TestTrue(TEXT("unkeyed spine scale uses the reference pose"),
            SpineTransforms[2].GetScale3D().Equals(FVector(1.5f, 2.0f, 0.5f), 0.01f));

        const float MidYaw = FMath::Abs(FRotator::NormalizeAxis(
            SpineTransforms[2].GetRotation().Rotator().Yaw));
        TestTrue(TEXT("350-to-10 rotation bakes through the short quaternion arc"),
            MidYaw < 5.0f);
    }
    return true;
#endif
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwAnimCompilerHeldPoseTest,
    "PinWright.Animation.Compiler.HeldPoseTrackCompilesAsOneKey",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

// A bone that holds one pose for the whole animation is the documented single-key form, and it
// is also what any bone gets when every key it declares carries the same value.  UE 5.8's
// sequencer data model stores such a track with ONE key (AnimSequencerController.cpp,
// `bAllKeysAreConstant`), so a readback assertion of "frames + 1 keys on every track" rejects a
// perfectly correct write.  It did, for every held pose, and it rejected it AFTER saving.
bool FPwAnimCompilerHeldPoseTest::RunTest(const FString& Parameters)
{
#if UE_VERSION_OLDER_THAN(5, 5, 0)
    AddWarning(TEXT("PINWRIGHT_ASSERTIONS_SKIPPED: HeldPoseTrackCompilesAsOneKey needs the "
        "UE 5.5+ authoring bracket; the engine asserts in anim compression on 5.3/5.4."));
    return true;
#else
    using namespace AnimAuthoringTestFixtures;

    const FName MovingBone(TEXT("pelvis"));
    const FName HeldBone(TEXT("spine_01"));
    const FTransform HeldRefPose(FRotator(0.0f, 0.0f, 0.0f), FVector(3.0f, 4.0f, 5.0f),
                                 FVector::OneVector);
    USkeleton* Skeleton = NewTransientSkeletonWithBones(
        { MovingBone, HeldBone }, { FTransform::Identity, HeldRefPose });
    FScopedAnimAssetRoot SkeletonRoot(Skeleton);
    TestNotNull(TEXT("Transient skeleton created"), Skeleton);
    if (!Skeleton)
    {
        return false;
    }
    const FString OutputAssetPath = FString::Printf(
        TEXT("/Game/PinWrightTests/%s"),
        *MakeUniqueAnimSeqTestAssetName(TEXT("AS_PwAnimHeldPose")));
    const FString Source = PwAnimCompilerTest_Source(
        Skeleton->GetPathName(),
        TEXT("bone \"pelvis\" {\n")
        TEXT("    key frame=0 at=(0, 0, 0)\n")
        TEXT("    key frame=4 at=(0, 0, 8)\n")
        TEXT("}\n")
        TEXT("bone \"spine_01\" {\n")
        TEXT("    key frame=0 rotate=(0, 0, 30)\n")
        TEXT("}\n"),
        4, false);

    FPwAnimCompileOptions Options;
    Options.OutputAssetPath = OutputAssetPath;
    Options.bSave = false;
    const FPwAnimCompileResult Result = FPwAnimCompiler::Compile(Source, Options);

    TestTrue(*FString::Printf(TEXT("a held-pose bone does not fail the compile. [%s]"),
        *JoinPwDiagnostics(Result.Diagnostics)), Result.bSuccess);
    if (!Result.bSuccess)
    {
        return false;
    }

    TestEqual(TEXT("both tracks reach the model"), Result.AssetTrackCount, 2);
    const int32 HeldIndex = Result.AssetTrackNames.IndexOfByKey(HeldBone);
    const int32 MovingIndex = Result.AssetTrackNames.IndexOfByKey(MovingBone);
    TestTrue(TEXT("both track names are reported"), HeldIndex != INDEX_NONE
        && MovingIndex != INDEX_NONE);
    if (HeldIndex == INDEX_NONE || MovingIndex == INDEX_NONE)
    {
        return false;
    }
    TestEqual(TEXT("the moving track keeps one key per frame"),
        Result.AssetKeyCountPerTrack[MovingIndex], 5);
    TestEqual(TEXT("the engine reduces the held track to one key"),
        Result.AssetKeyCountPerTrack[HeldIndex], 1);
    TestEqual(TEXT("and that reduction is what the compiler expected"),
        Result.ExpectedKeyCountPerTrack[HeldIndex], 1);

    // The count is not the point; the evaluated pose is.  A one-key track must still read back
    // the authored value at every frame, reference-pose translation and scale included.
    UAnimSequence* Sequence = LoadObject<UAnimSequence>(nullptr, *Result.AssetPath);
    TestNotNull(TEXT("generated AnimSequence loads"), Sequence);
    if (!Sequence)
    {
        return false;
    }
    Sequence->AddToRoot();
    ON_SCOPE_EXIT { Sequence->RemoveFromRoot(); };

    TArray<FFrameNumber> Frames;
    for (int32 Frame = 0; Frame <= 4; ++Frame)
    {
        Frames.Add(FFrameNumber(Frame));
    }
    TArray<FTransform> HeldTransforms;
    Sequence->GetDataModel()->GetBoneTrackTransforms(HeldBone, Frames, HeldTransforms);
    TestEqual(TEXT("a one-key track still evaluates at every frame"), HeldTransforms.Num(), 5);
    for (int32 Frame = 0; Frame < HeldTransforms.Num(); ++Frame)
    {
        TestTrue(*FString::Printf(TEXT("held rotation survives at frame %d"), Frame),
            FMath::IsNearlyEqual(FRotator::NormalizeAxis(
                HeldTransforms[Frame].GetRotation().Rotator().Yaw), 30.0f, 0.05f));
        TestTrue(*FString::Printf(TEXT("held translation is the reference pose at frame %d"), Frame),
            HeldTransforms[Frame].GetTranslation().Equals(FVector(3.0f, 4.0f, 5.0f), 0.01f));
    }
    return true;
#endif
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwAnimCompilerValidationDiagnosticsTest,
    "PinWright.Animation.Compiler.ReportsSkeletonAndLoopDiagnostics",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwAnimCompilerLoopFlagTest,
    "PinWright.Animation.Compiler.PropagatesLoopFlag",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwAnimCompilerLoopFlagTest::RunTest(const FString& Parameters)
{
#if UE_VERSION_OLDER_THAN(5, 5, 0)
    PinWrightTestSkip::SkipAssertions(*this, TEXT("engine-version-unsupported"),
        TEXT("Skipping PropagatesLoopFlag on UE 5.3/5.4: generated AnimSequence authoring "
            "can trigger the engine's anim-compression assert while closing the controller bracket."));
    return true;
#else
    using namespace AnimAuthoringTestFixtures;

    const FName PelvisBone(TEXT("pelvis"));
    USkeleton* Skeleton = NewTransientSkeletonWithBones({ PelvisBone });
    FScopedAnimAssetRoot SkeletonRoot(Skeleton);
    TestNotNull(TEXT("Transient skeleton for loop flag created"), Skeleton);
    if (!Skeleton)
    {
        return false;
    }
    const FString LoopSource = PwAnimCompilerTest_Source(
        Skeleton->GetPathName(),
        TEXT("bone \"pelvis\" {\n")
        TEXT("    key frame=0 at=(0, 0, 0)\n")
        TEXT("    key frame=1 at=(0, 0, 1)\n")
        TEXT("    key frame=2 at=(0, 0, 0)\n")
        TEXT("}\n"),
        2, true);

    FPwAnimCompileOptions Options;
    Options.OutputAssetPath = FString::Printf(
        TEXT("/Game/PinWrightTests/%s"),
        *MakeUniqueAnimSeqTestAssetName(TEXT("AS_PwAnimLoopFlag")));
    Options.bSave = false;
    const FPwAnimCompileResult Result = FPwAnimCompiler::Compile(LoopSource, Options);
    TestTrue(*FString::Printf(TEXT("looping source compiles. [%s]"),
        *JoinPwDiagnostics(Result.Diagnostics)), Result.bSuccess);
    TestTrue(TEXT("compiler reports the requested loop flag"), Result.bLoop);
    if (!Result.bSuccess)
    {
        return false;
    }

    UAnimSequence* Sequence = LoadObject<UAnimSequence>(nullptr, *Result.AssetPath);
    TestNotNull(TEXT("generated loop animation loads"), Sequence);
    if (Sequence)
    {
        TestTrue(TEXT("compiled UAnimSequence carries bLoop=true"), Sequence->bLoop);
    }

    const FString OneShotSource = PwAnimCompilerTest_Source(
        Skeleton->GetPathName(),
        TEXT("bone \"pelvis\" {\n")
        TEXT("    key frame=0 at=(0, 0, 0)\n")
        TEXT("    key frame=2 at=(0, 0, 1)\n")
        TEXT("}\n"),
        2, false);
    FPwAnimCompileOptions OneShotOptions;
    OneShotOptions.OutputAssetPath = FString::Printf(
        TEXT("/Game/PinWrightTests/%s"),
        *MakeUniqueAnimSeqTestAssetName(TEXT("AS_PwAnimOneShotFlag")));
    OneShotOptions.bSave = false;
    const FPwAnimCompileResult OneShotResult = FPwAnimCompiler::Compile(
        OneShotSource, OneShotOptions);
    TestTrue(*FString::Printf(TEXT("one-shot source compiles. [%s]"),
        *JoinPwDiagnostics(OneShotResult.Diagnostics)), OneShotResult.bSuccess);
    TestFalse(TEXT("one-shot source reports loop=false"), OneShotResult.bLoop);
    TestNull(TEXT("loop=false suppresses the seam diagnostic"),
        PwAnimCompilerTest_FindCode(OneShotResult.Diagnostics,
            PwAnimDiagnosticCodes::PWANIM_LOOP_SEAM));
    if (OneShotResult.bSuccess)
    {
        UAnimSequence* OneShotSequence = LoadObject<UAnimSequence>(nullptr, *OneShotResult.AssetPath);
        TestNotNull(TEXT("generated one-shot animation loads"), OneShotSequence);
        if (OneShotSequence)
        {
            TestFalse(TEXT("compiled UAnimSequence carries bLoop=false"), OneShotSequence->bLoop);
            TestEqual(TEXT("compiled one-shot UAnimSequence uses linear interpolation"),
                OneShotSequence->Interpolation, EAnimInterpolationType::Linear);
        }
    }
    return true;
#endif
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwAnimCompilerSyncMarkerAuthoringTest,
    "PinWright.Animation.Compiler.SyncMarkersAreIdempotentAndRefreshDerivedState",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwAnimCompilerSyncMarkerAuthoringTest::RunTest(const FString& Parameters)
{
#if UE_VERSION_OLDER_THAN(5, 5, 0)
    PinWrightTestSkip::SkipAssertions(*this, TEXT("engine-version-unsupported"),
        TEXT("Skipping SyncMarkersAreIdempotentAndRefreshDerivedState on UE 5.3/5.4: "
            "generated AnimSequence authoring can trigger the engine's anim-compression assert."));
    return true;
#else
    using namespace AnimAuthoringTestFixtures;

    USkeleton* Skeleton = NewTransientSkeletonWithBones({ FName(TEXT("root")) });
    FScopedAnimAssetRoot SkeletonRoot(Skeleton);
    TestNotNull(TEXT("Transient skeleton for sync-marker source created"), Skeleton);
    if (!Skeleton)
    {
        return false;
    }
    const FString OutputAssetPath = FString::Printf(
        TEXT("/Game/PinWrightTests/%s"),
        *MakeUniqueAnimSeqTestAssetName(TEXT("AS_PwAnimSyncMarkers")));
    const FString SourcePath = TEXT("Saved/PinWright/Tests/sync-markers.pwanim");
    const FString SourceWithMarkers = FString::Printf(
        TEXT("pwanim 0\n")
        TEXT("use skeleton from \"%s\"\n")
        TEXT("timebase rate=(30, 1) frames=4\n")
        TEXT("sync_marker \"L\" frame=0\n")
        TEXT("sync_marker \"R\" frame=2\n")
        TEXT("sync_marker \"L\" frame=4\n")
        TEXT("bone \"root\" {\n")
        TEXT("    key frame=0 at=(0, 0, 0)\n")
        TEXT("}\n"),
        *Skeleton->GetPathName());

    FPwAnimCompileOptions Options;
    Options.OutputAssetPath = OutputAssetPath;
    Options.SourcePath = SourcePath;
    Options.bSave = false;

    const FPwAnimCompileResult First = FPwAnimCompiler::Compile(SourceWithMarkers, Options);
    TestTrue(*FString::Printf(TEXT("source with sync markers compiles. [%s]"),
        *JoinPwDiagnostics(First.Diagnostics)), First.bSuccess);
    if (!First.bSuccess)
    {
        return false;
    }
    TestEqual(TEXT("first compile read back all authored markers"),
        First.AssetSyncMarkers.Num(), 3);

    UAnimSequence* Sequence = LoadObject<UAnimSequence>(nullptr, *First.AssetPath);
    TestNotNull(TEXT("compiled sequence loads for marker readback"), Sequence);
    if (!Sequence)
    {
        return false;
    }
    Sequence->AddToRoot();
    ON_SCOPE_EXIT { Sequence->RemoveFromRoot(); };

    TestEqual(TEXT("first compile authors three markers"), Sequence->AuthoredSyncMarkers.Num(), 3);
    TestEqual(TEXT("derived unique marker names contain two labels"),
        Sequence->UniqueMarkerNames.Num(), 2);
    TestTrue(TEXT("derived marker track exists"), Sequence->AnimNotifyTracks.Num() > 0);
    if (Sequence->AnimNotifyTracks.Num() > 0)
    {
        TestEqual(TEXT("derived marker track links contain all markers"),
            Sequence->AnimNotifyTracks[0].SyncMarkers.Num(), 3);
    }
    for (const FAnimSyncMarker& Marker : Sequence->AuthoredSyncMarkers)
    {
        TestEqual(TEXT("authored marker track index is refreshed to zero"), Marker.TrackIndex, 0);
    }

    const FPwAnimCompileResult Second = FPwAnimCompiler::Compile(SourceWithMarkers, Options);
    TestTrue(*FString::Printf(TEXT("recompiling the same source succeeds. [%s]"),
        *JoinPwDiagnostics(Second.Diagnostics)), Second.bSuccess);
    TestEqual(TEXT("recompile does not append duplicate markers"),
        Sequence->AuthoredSyncMarkers.Num(), 3);
    TestEqual(TEXT("recompile readback remains three markers"),
        Second.AssetSyncMarkers.Num(), 3);

    const FString SourceWithoutMarkers = FString::Printf(
        TEXT("pwanim 0\n")
        TEXT("use skeleton from \"%s\"\n")
        TEXT("timebase rate=(30, 1) frames=4\n")
        TEXT("bone \"root\" {\n")
        TEXT("    key frame=0 at=(0, 0, 0)\n")
        TEXT("}\n"),
        *Skeleton->GetPathName());
    const FPwAnimCompileResult WithoutMarkers = FPwAnimCompiler::Compile(
        SourceWithoutMarkers, Options);
    TestTrue(*FString::Printf(TEXT("source with no marker statement still compiles. [%s]"),
        *JoinPwDiagnostics(WithoutMarkers.Diagnostics)), WithoutMarkers.bSuccess);
    TestEqual(TEXT("omitting marker syntax deterministically clears authored markers"),
        Sequence->AuthoredSyncMarkers.Num(), 0);
    TestEqual(TEXT("source readback reports the empty marker set"),
        WithoutMarkers.AssetSyncMarkers.Num(), 0);
    return true;
#endif
}

bool FPwAnimCompilerValidationDiagnosticsTest::RunTest(const FString& Parameters)
{
    using namespace AnimAuthoringTestFixtures;

    const FName PelvisBone(TEXT("pelvis"));
    const FName SpineBone(TEXT("spine_01"));
    USkeleton* Skeleton = NewTransientSkeletonWithBones({ PelvisBone, SpineBone });
    FScopedAnimAssetRoot SkeletonRoot(Skeleton);
    TestNotNull(TEXT("Transient skeleton for validation diagnostics created"), Skeleton);
    if (!Skeleton)
    {
        return false;
    }
    {
        FPwAnimCompileOptions Options;
        Options.bValidateOnly = true;
        const FString Source = PwAnimCompilerTest_Source(
            Skeleton->GetPathName(),
            TEXT("bone \"spine_1\" {\n")
            TEXT("    key frame=0 at=(0, 0, 0)\n")
            TEXT("}\n"),
            1, false);
        const FPwAnimCompileResult Result = FPwAnimCompiler::Compile(Source, Options);

        const FPwDiagnostic* Diagnostic = PwAnimCompilerTest_FindCode(Result.Diagnostics,
            PwAnimDiagnosticCodes::PWANIM_UNKNOWN_BONE);
        TestNotNull(TEXT("unknown bone is diagnosed"), Diagnostic);
        if (Diagnostic)
        {
            TestTrue(TEXT("unknown bone suggests the closest reference bone"),
                Diagnostic->Suggestions.Contains(TEXT("spine_01")));
        }
    }

    {
        FPwAnimCompileOptions Options;
        Options.bValidateOnly = true;
        const FString Source = PwAnimCompilerTest_Source(
            Skeleton->GetPathName(),
            TEXT("bone \"pelvis\" {\n")
            TEXT("    key frame=0 at=(0, 0, 0)\n")
            TEXT("    key frame=2 at=(0, 0, 1)\n")
            TEXT("}\n"),
            2, true);
        const FPwAnimCompileResult Result = FPwAnimCompiler::Compile(Source, Options);

        TestNotNull(TEXT("non-closing loop is diagnosed"), PwAnimCompilerTest_FindCode(
            Result.Diagnostics, PwAnimDiagnosticCodes::PWANIM_LOOP_SEAM));
    }

    {
        FPwAnimCompileOptions Options;
        Options.bValidateOnly = true;
        const FString Source = PwAnimCompilerTest_Source(
            TEXT("/Game/PinWrightTests/DoesNotExist_SK"),
            TEXT("bone \"pelvis\" {\n")
            TEXT("    key frame=0 at=(0, 0, 0)\n")
            TEXT("}\n"),
            1, false);
        const FPwAnimCompileResult Result = FPwAnimCompiler::Compile(Source, Options);

        TestNotNull(TEXT("missing skeleton is reported by the shared resolver"),
            PwAnimCompilerTest_FindCode(Result.Diagnostics,
                PwAnimDiagnosticCodes::PWANIM_SKELETON_NOT_FOUND));
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwAnimCompilerProvenanceDiagnosticSplitTest,
    "PinWright.Animation.Compiler.ProvenanceRefusalIsNotABadValue",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwAnimCompilerProvenanceDiagnosticSplitTest::RunTest(const FString& Parameters)
{
#if UE_VERSION_OLDER_THAN(5, 5, 0)
    AddWarning(TEXT("PINWRIGHT_ASSERTIONS_SKIPPED: ProvenanceRefusalIsNotABadValue needs the "
        "UE 5.5+ animation authoring bracket."));
    return true;
#else
    using namespace AnimAuthoringTestFixtures;

    USkeleton* Skeleton = NewTransientSkeletonWithBones({ FName(TEXT("root")) });
    FScopedAnimAssetRoot SkeletonRoot(Skeleton);
    if (!TestNotNull(TEXT("transient skeleton for provenance split created"), Skeleton))
    {
        return false;
    }
    const FString OutputAssetPath = FString::Printf(
        TEXT("/Game/PinWrightTests/%s"),
        *MakeUniqueAnimSeqTestAssetName(TEXT("AS_PwAnimProvenance")));
    const FString Source = PwAnimCompilerTest_Source(
        Skeleton->GetPathName(),
        TEXT("bone \"root\" {\n")
        TEXT("    key frame=0 at=(0, 0, 0)\n")
        TEXT("}\n"),
        1, false);

    FPwAnimCompileOptions OriginalOptions;
    OriginalOptions.OutputAssetPath = OutputAssetPath;
    OriginalOptions.SourcePath = TEXT("Tests/Owners/original.pwanim");
    OriginalOptions.bSave = false;
    const FPwAnimCompileResult First = FPwAnimCompiler::Compile(Source, OriginalOptions);
    if (!TestTrue(*FString::Printf(TEXT("the original animation compiles. [%s]"),
            *JoinPwDiagnostics(First.Diagnostics)), First.bSuccess))
    {
        return false;
    }

    UAnimSequence* Sequence = LoadObject<UAnimSequence>(nullptr, *First.AssetPath);
    if (!TestNotNull(TEXT("the generated sequence is available for a live-state edit"), Sequence))
    {
        return false;
    }
    FAnimNotifyEvent Notify;
    Notify.NotifyName = FName(TEXT("Checkpoint"));
    Notify.SetTime(0.0f);
    Sequence->Notifies.Add(Notify);

    FPwAnimCompileOptions TakeoverOptions = OriginalOptions;
    TakeoverOptions.SourcePath = TEXT("Tests/Owners/replacement.pwanim");
    const FPwAnimCompileResult Refused = FPwAnimCompiler::Compile(Source, TakeoverOptions);
    TestFalse(TEXT("a different animation source needs overwrite permission"), Refused.bSuccess);
    const FPwDiagnostic* Provenance = PwAnimCompilerTest_FindCode(Refused.Diagnostics,
        PwAnimDiagnosticCodes::PWANIM_ASSET_PROVENANCE_CONFLICT);
    TestNotNull(TEXT("the ownership refusal has its format-specific code"), Provenance);
    if (Provenance)
    {
        TestTrue(TEXT("the ownership diagnostic names the current source"),
            Provenance->Message.Contains(OriginalOptions.SourcePath));
    }
    TestNull(TEXT("the ownership refusal is not classified as a malformed value"),
        PwAnimCompilerTest_FindCode(Refused.Diagnostics,
            PwSourceDiagnosticCodes::PWSRC_BAD_VALUE));
    const FPwDiagnostic* Unmanaged = PwAnimCompilerTest_FindCode(Refused.Diagnostics,
        PwSourceDiagnosticCodes::PWSRC_RECOMPILE_UNMANAGED_STATE);
    TestNotNull(TEXT("the animation takeover also runs the unmanaged-state guard"), Unmanaged);
    if (Unmanaged)
    {
        TestTrue(TEXT("the takeover refusal names the live notify that would be lost"),
            Unmanaged->Message.Contains(TEXT("notify[0]")));
    }

    FPwAnimCompileOptions MalformedOptions;
    MalformedOptions.bValidateOnly = true;
    const FString Malformed = PwAnimCompilerTest_Source(
        Skeleton->GetPathName(),
        TEXT("bone \"root\" {\n")
        TEXT("    key frame=0 at=(0, 0, 0)\n")
        TEXT("}\n"),
        0, false);
    const FPwAnimCompileResult BadValue = FPwAnimCompiler::Compile(Malformed, MalformedOptions);
    TestFalse(TEXT("a zero-frame timebase remains malformed"), BadValue.bSuccess);
    TestNotNull(TEXT("a genuine malformed value keeps PWSRC_BAD_VALUE"),
        PwAnimCompilerTest_FindCode(BadValue.Diagnostics,
            PwSourceDiagnosticCodes::PWSRC_BAD_VALUE));
    TestNull(TEXT("a malformed value is not classified as an ownership conflict"),
        PwAnimCompilerTest_FindCode(BadValue.Diagnostics,
            PwAnimDiagnosticCodes::PWANIM_ASSET_PROVENANCE_CONFLICT));
    return true;
#endif
}
