// Copyright (c) 2026 Alexander Penkin. MIT License.

// Failure-direction coverage for compile and dump boundaries that must never report a write
// which the resulting editor state does not support.
#include "Misc/AutomationTest.h"

#include "Handlers/Animation/AnimSequenceCreate.h"
#include "PwAnim/PwAnimCompiler.h"
#include "PwSkel/PwSkelAssetCreate.h"
#include "PwSkel/PwSkelDiagnostic.h"
#include "PwSkel/PwSkelParser.h"
#include "Utils/AssetDumpWriter.h"
#include "Tests/TestSkipReporting.h"

#include "Animation/AnimSequence.h"
#include "Animation/Skeleton.h"
#include "FileHelpers.h"
#include "HAL/FileManager.h"
#include "Misc/EngineVersionComparison.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"

namespace PinWrightVerbTruthfulnessTests
{
    FString UniqueAssetPath(const TCHAR* Stem)
    {
        return FString::Printf(TEXT("/Game/PinWrightTests/%s_%s"), Stem,
            *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    }

    FString SkeletonSource(int32 BoneCount)
    {
        FString Source(TEXT("pwskel 0\n"));
        for (int32 Index = 0; Index < BoneCount; ++Index)
        {
            Source += FString::Printf(TEXT("%sbone \"bone_%02d\" {\n"),
                *FString::ChrN(Index * 2, TEXT(' ')), Index);
        }
        for (int32 Index = BoneCount - 1; Index >= 0; --Index)
        {
            Source += FString::Printf(TEXT("%s}\n"),
                *FString::ChrN(Index * 2, TEXT(' ')));
        }
        return Source;
    }

    bool ParseSkeleton(const FString& Source, FPwSkelDocument& OutDocument,
                       FAutomationTestBase& Test)
    {
        TArray<FPwDiagnostic> Diagnostics;
        const bool bParsed = FPwSkelParser::Parse(Source, OutDocument, Diagnostics);
        if (!bParsed)
        {
            Test.AddError(JoinPwDiagnostics(Diagnostics));
        }
        return bParsed;
    }

    FSkeletonCreateSpec SkeletonSpec(const FString& AssetPath, const FString& SourcePath)
    {
        FSkeletonCreateSpec Spec;
        Spec.AssetPath = AssetPath;
        Spec.SourcePath = SourcePath;
        Spec.SourceHash = TEXT("test");
        Spec.bSave = false;
        return Spec;
    }

    int32 BoneTreeCount(USkeleton* Skeleton)
    {
        FArrayProperty* Property = FindFProperty<FArrayProperty>(
            USkeleton::StaticClass(), TEXT("BoneTree"));
        if (!Property || !Skeleton)
        {
            return INDEX_NONE;
        }
        FScriptArrayHelper Helper(Property, Property->ContainerPtrToValuePtr<void>(Skeleton));
        return Helper.Num();
    }

    void ResizeBoneTree(USkeleton* Skeleton, int32 Count)
    {
        FArrayProperty* Property = FindFProperty<FArrayProperty>(
            USkeleton::StaticClass(), TEXT("BoneTree"));
        if (Property && Skeleton)
        {
            FScriptArrayHelper Helper(Property, Property->ContainerPtrToValuePtr<void>(Skeleton));
            Helper.Resize(Count);
        }
    }

    FString AnimationSource(const FString& SkeletonPath, const TCHAR* BoneName,
                            double EndPosition = 1.0)
    {
        return FString::Printf(
            TEXT("pwanim 0\nuse skeleton from \"%s\"\n")
            TEXT("timebase rate=(30, 1) frames=1\n")
            TEXT("bone \"%s\" {\n")
            TEXT("  key frame=0 at=(0, 0, 0)\n")
            TEXT("  key frame=1 at=(%.9g, 0, 0)\n")
            TEXT("}\n"), *SkeletonPath, BoneName, EndPosition);
    }

    FPwAnimCompileOptions AnimationOptions(
        const FString& AssetPath, const FString& SourcePath, bool bValidateOnly = false)
    {
        FPwAnimCompileOptions Options;
        Options.OutputAssetPath = AssetPath;
        Options.SourcePath = SourcePath;
        Options.SourceHash = TEXT("test");
        Options.bSave = false;
        Options.bValidateOnly = bValidateOnly;
        return Options;
    }

    void ClearDirty(UObject* Object)
    {
        if (Object && Object->GetOutermost())
        {
            Object->GetOutermost()->SetDirtyFlag(false);
        }
    }

    float TrackLastPositionX(const UAnimSequence* Sequence, FName BoneName)
    {
        float Result = TNumericLimits<float>::Lowest();
        if (Sequence && Sequence->GetDataModel())
        {
            Sequence->GetDataModel()->IterateBoneKeys(BoneName,
                [&Result](const FVector3f& Position, const FQuat4f&, const FVector3f&,
                          const FFrameNumber&)
                {
                    Result = Position.X;
                    return true;
                });
        }
        return Result;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSkeletonCompilePostConditionTest,
    "PinWright.Skeleton.Compile.IncompleteAssetFails",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSkeletonCompilePostConditionTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightVerbTruthfulnessTests;
    const FString AssetPath = UniqueAssetPath(TEXT("PW_PostConditionSkeleton"));
    const FString SourcePath = TEXT("Tests/PostCondition.pwskel");

    FPwSkelDocument Initial;
    FPwSkelDocument Expanded;
    if (!ParseSkeleton(SkeletonSource(21), Initial, *this)
        || !ParseSkeleton(SkeletonSource(24), Expanded, *this))
    {
        return false;
    }

    const FSkeletonCreateResult First = CreateSkeleton(Initial, SkeletonSpec(AssetPath, SourcePath));
    if (!TestTrue(TEXT("the 21-bone setup compiles"), First.bSuccess))
    {
        return false;
    }
    ClearDirty(First.Asset);

    const FSkeletonCreateResult Organic = CreateSkeleton(
        Expanded, SkeletonSpec(AssetPath, SourcePath));
    TestTrue(TEXT("the ordinary 24-bone recompile completes"), Organic.bSuccess);
    TestEqual(TEXT("the ordinary recompile has 24 reference bones"),
        Organic.Asset ? Organic.Asset->GetReferenceSkeleton().GetRawBoneNum() : INDEX_NONE, 24);
    TestEqual(TEXT("the ordinary recompile has 24 BoneTree entries"),
        BoneTreeCount(Organic.Asset), 24);
    ClearDirty(Organic.Asset);

    FSkeletonCreateSpec IncompleteSpec = SkeletonSpec(AssetPath, SourcePath);
    IncompleteSpec.BeforePostConditionForTest = [](USkeleton* Skeleton)
    {
        ResizeBoneTree(Skeleton, 21);
    };
    const FSkeletonCreateResult Incomplete = CreateSkeleton(Expanded, IncompleteSpec);

    TestFalse(TEXT("an incomplete asset must not report compile success"), Incomplete.bSuccess);
    TestTrue(TEXT("the failure carries the exact post-condition diagnostic"),
        Incomplete.Diagnostics.ContainsByPredicate([](const FPwDiagnostic& Diagnostic)
        {
            return Diagnostic.Code ==
                PwSkelDiagnosticCodes::PWSKEL_ASSET_POSTCONDITION_FAILED;
        }));
    ClearDirty(Incomplete.Asset ? Incomplete.Asset : Organic.Asset);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAnimCompileSameSessionSkeletonRefreshTest,
    "PinWright.anim.Compile.SameSessionSkeletonRefresh",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAnimCompileSameSessionSkeletonRefreshTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightVerbTruthfulnessTests;
    const FString SkeletonPath = UniqueAssetPath(TEXT("PW_RefreshSkeleton"));
    const FString SkeletonSourcePath = TEXT("Tests/RefreshSkeleton.pwskel");
    const FString AnimationPath = UniqueAssetPath(TEXT("PW_RefreshAnimation"));
    const FString AnimationSourcePath = TEXT("Tests/RefreshAnimation.pwanim");

    FPwSkelDocument Initial;
    FPwSkelDocument Expanded;
    if (!ParseSkeleton(SkeletonSource(1), Initial, *this)
        || !ParseSkeleton(SkeletonSource(2), Expanded, *this))
    {
        return false;
    }
    const FSkeletonCreateResult FirstSkeleton = CreateSkeleton(
        Initial, SkeletonSpec(SkeletonPath, SkeletonSourcePath));
    if (!TestTrue(TEXT("the initial skeleton compiles"), FirstSkeleton.bSuccess))
    {
        return false;
    }

    const FPwAnimCompileResult FirstAnimation = FPwAnimCompiler::Compile(
        AnimationSource(FirstSkeleton.Asset->GetPathName(), TEXT("bone_00")),
        AnimationOptions(AnimationPath, AnimationSourcePath));
    if (!TestTrue(TEXT("the loaded animation fixture compiles"), FirstAnimation.bSuccess))
    {
        return false;
    }
    UAnimSequence* LoadedAnimation = LoadObject<UAnimSequence>(nullptr, *FirstAnimation.AssetPath);
    if (!TestNotNull(TEXT("the animation stays loaded"), LoadedAnimation))
    {
        return false;
    }
    const FGuid PreviousSkeletonGuid = FirstSkeleton.Asset->GetGuid();

    const FSkeletonCreateResult SecondSkeleton = CreateSkeleton(
        Expanded, SkeletonSpec(SkeletonPath, SkeletonSourcePath));
    TestTrue(TEXT("the expanded skeleton compiles"), SecondSkeleton.bSuccess);
    TestNotEqual(TEXT("a hierarchy change regenerates the skeleton generation"),
        SecondSkeleton.Asset->GetGuid(), PreviousSkeletonGuid);
    TestEqual(TEXT("the loaded animation observes the refreshed generation"),
        LoadedAnimation->GetSkeletonGuid(), SecondSkeleton.Asset->GetGuid());

#if UE_VERSION_OLDER_THAN(5, 7, 0)
    // The remaining half asserts an engine capability UE 5.6 and older do not have. The
    // animation's data model owns an FK control-rig hierarchy built from the skeleton it was
    // first compiled against; a bone ADDED to that skeleton afterwards never reaches it, because
    // IAnimationDataController::UpdateWithSkeleton regenerates the hierarchy only for bones
    // REMOVED (UE 5.7 added the BonesToBeAdded branch) and InitializeModel builds nothing once
    // the model has a MovieScene. The sequencer controller then refuses the new bone's track
    // ("Unable to find RigBone with name ..."), so the recompile cannot succeed on this engine
    // without an editor restart. Replacing the model wholesale is not a workaround: the only
    // public route, UAnimSequence::CreateAnimation, copies a model that must already be
    // initialized, and the copy loses the Control Rig section.
    PinWrightTestSkip::SkipAssertions(*this, TEXT("engine-cannot-add-bone-in-session"),
        TEXT("Skipping the same-session new-bone recompile on UE 5.6 and older: "
            "UpdateWithSkeleton does not regenerate the data model's bone hierarchy for added bones."));
    return true;
#else
    const FString NewBoneSource = AnimationSource(
        SecondSkeleton.Asset->GetPathName(), TEXT("bone_01"));
    const FPwAnimCompileResult Validated = FPwAnimCompiler::Compile(
        NewBoneSource, AnimationOptions(AnimationPath, AnimationSourcePath, true));
    const FPwAnimCompileResult Compiled = FPwAnimCompiler::Compile(
        NewBoneSource, AnimationOptions(AnimationPath, AnimationSourcePath));
    TestTrue(TEXT("validation accepts the new bone"), Validated.bSuccess);
    TestTrue(TEXT("compile accepts the same new bone without a restart"), Compiled.bSuccess);
    TestEqual(TEXT("validate and compile resolve the same skeleton path"),
        Compiled.SkeletonPath, Validated.SkeletonPath);
    TestEqual(TEXT("validate and compile resolve the same bone count"),
        Compiled.SkeletonBoneCount, Validated.SkeletonBoneCount);
    TestEqual(TEXT("both calls observe two bones"), Compiled.SkeletonBoneCount, 2);
    return true;
#endif
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAnimCompileFailureRollbackTest,
    "PinWright.anim.Compile.FailureLeavesNoDirtyPackages",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAnimCompileFailureRollbackTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightVerbTruthfulnessTests;
    const FString SkeletonPath = UniqueAssetPath(TEXT("PW_RollbackSkeleton"));
    FPwSkelDocument SkeletonDocument;
    if (!ParseSkeleton(SkeletonSource(1), SkeletonDocument, *this))
    {
        return false;
    }
    const FSkeletonCreateResult Skeleton = CreateSkeleton(
        SkeletonDocument, SkeletonSpec(SkeletonPath, TEXT("Tests/Rollback.pwskel")));
    if (!TestTrue(TEXT("the rollback skeleton compiles"), Skeleton.bSuccess))
    {
        return false;
    }

    const FString AnimationPath = UniqueAssetPath(TEXT("PW_RollbackAnimation"));
    const FString SourcePath = TEXT("Tests/Rollback.pwanim");
    const FString OriginalSource = AnimationSource(Skeleton.Asset->GetPathName(), TEXT("bone_00"), 1.0);
    const FPwAnimCompileResult First = FPwAnimCompiler::Compile(
        OriginalSource, AnimationOptions(AnimationPath, SourcePath));
    if (!TestTrue(TEXT("the initial animation compiles"), First.bSuccess))
    {
        return false;
    }
    UAnimSequence* Sequence = LoadObject<UAnimSequence>(nullptr, *First.AssetPath);
    if (!TestNotNull(TEXT("the initial animation loads"), Sequence))
    {
        return false;
    }
    const int32 OriginalFrames = Sequence->GetDataModel()->GetNumberOfFrames();
    const float OriginalEndX = TrackLastPositionX(Sequence, FName(TEXT("bone_00")));
    ClearDirty(Skeleton.Asset);
    ClearDirty(Sequence);

    // MEASURED AS A DELTA, NOT AS AN ABSOLUTE. FEditorFileUtils::GetDirtyContentPackages reads a
    // PROCESS-WIDE set, so in a full-suite run this test inherits every /Game package an earlier
    // test left dirty - 40 of them on the UE 5.4 run that made this concrete (ABP_AGIR_* fixtures
    // and __PW_GatewayTests probes, none of which this test creates, loads or compiles). An
    // absolute zero therefore measures the suite's history rather than this compile's rollback,
    // and it passes or fails on test ORDER. The claim being made is that the FAILED COMPILE
    // dirties nothing, which is exactly what the before/after difference below says - and it
    // still catches the regression it was written for, because the two packages this compile can
    // touch were just cleared above and so cannot hide inside the inherited set.
    TArray<UPackage*> DirtyBeforeCompile;
    FEditorFileUtils::GetDirtyContentPackages(DirtyBeforeCompile);
    const TSet<UPackage*> InheritedDirty(DirtyBeforeCompile);

    FPwAnimCompileOptions FailingOptions = AnimationOptions(AnimationPath, SourcePath);
    FailingOptions.PostWriteCheckForTest = [](UAnimSequence*) { return false; };
    const FPwAnimCompileResult Failed = FPwAnimCompiler::Compile(
        AnimationSource(Skeleton.Asset->GetPathName(), TEXT("bone_00"), 25.0),
        FailingOptions);
    TestFalse(TEXT("the injected post-write failure is reported"), Failed.bSuccess);
    TestEqual(TEXT("the failed compile restores the previous frame count"),
        Sequence->GetDataModel()->GetNumberOfFrames(), OriginalFrames);
    TestEqual(TEXT("the failed compile restores the previous track keys"),
        TrackLastPositionX(Sequence, FName(TEXT("bone_00"))), OriginalEndX);

    TArray<UPackage*> DirtyPackages;
    FEditorFileUtils::GetDirtyContentPackages(DirtyPackages);
    TArray<FString> NewlyDirty;
    for (UPackage* Package : DirtyPackages)
    {
        if (Package && !InheritedDirty.Contains(Package))
        {
            NewlyDirty.Add(Package->GetName());
        }
    }
    TestEqual(*FString::Printf(
            TEXT("a failed compile dirties no content package (newly dirty: %s)"),
            NewlyDirty.Num() > 0 ? *FString::Join(NewlyDirty, TEXT(", ")) : TEXT("none")),
        NewlyDirty.Num(), 0);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDumpWriterTrailingWhitespaceTest,
    "PinWright.asset.Dump.NoTrailingWhitespace",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetDumpWriterTrailingWhitespaceTest::RunTest(const FString& Parameters)
{
    const FString Root = FPaths::ProjectIntermediateDir() / TEXT("AssetDumpTests")
        / FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString DumpDir = Root / TEXT("Fixture");
    TArray<AssetDumpWriter::FDumpFile> Files;
    Files.Add({TEXT("sidecar.txt"), TEXT("alpha  \n beta\t\r\nblank \t\nlast   ")});

    FString Error;
    const AssetDumpWriter::FWriteResult Result =
        AssetDumpWriter::WriteAssetDump(DumpDir, Root, Files, Error);
    TestTrue(*FString::Printf(TEXT("the dump write succeeds: %s"), *Error), Error.IsEmpty());
    TestEqual(TEXT("one sidecar is written"), Result.WrittenPaths.Num(), 1);

    FString Written;
    FFileHelper::LoadFileToString(Written, *(DumpDir / TEXT("sidecar.txt")));
    TArray<FString> Lines;
    Written.ParseIntoArrayLines(Lines, false);
    for (int32 Index = 0; Index < Lines.Num(); ++Index)
    {
        TestFalse(*FString::Printf(TEXT("line %d does not end in whitespace"), Index + 1),
            Lines[Index].EndsWith(TEXT(" ")) || Lines[Index].EndsWith(TEXT("\t")));
    }

    IFileManager::Get().DeleteDirectory(*Root, false, true);
    return true;
}
