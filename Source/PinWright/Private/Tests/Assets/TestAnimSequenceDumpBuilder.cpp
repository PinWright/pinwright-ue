// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"
#include "Handlers/Asset/AssetDumpHandler.h"
#include "Handlers/Asset/AnimSequenceDumpBuilder.h"
#include "Handlers/Animation/AnimSequenceCreate.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Misc/PackageName.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetDumpTestHelpers.h"
#include "Tests/TestUtils.h"
#include "Tests/TestSkipReporting.h"
#include "Compat/EngineVersionCompat.h"
#include "AnimAuthoringTestFixtures.h"


#include "Animation/AnimSequence.h"
#include "Animation/AnimTypes.h"
#include "Animation/AnimData/IAnimationDataController.h"
#include "Animation/AnimData/IAnimationDataModel.h"
#include "Animation/Skeleton.h"
#include "ReferenceSkeleton.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/UObjectGlobals.h"

namespace
{
    using AssetDumpTestHelpers::HasDumpFile;
    using AssetDumpTestHelpers::FindDumpFile;
    using AssetDumpTestHelpers::LoadJsonFile;
    using namespace AnimAuthoringTestFixtures;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAnimSequenceDumpBuilderShapeTest,
    "PinWright.Assets.AnimSequence.DumpBuilder.Shape",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAnimSequenceDumpBuilderShapeTest::RunTest(const FString& Parameters)
{
    FString ObjectPath;
    UAnimSequence* Sequence = NewTransientAnimSequence(ObjectPath);
    FScopedAnimAssetRoot SequenceRoot(Sequence);
    TestNotNull(TEXT("Transient UAnimSequence created"), Sequence);
    if (!Sequence)
    {
        return false;
    }

    TSharedPtr<FJsonObject> Json = AnimSequenceDumpBuilder::BuildAnimSequenceJson(Sequence);
    TestTrue(TEXT("BuildAnimSequenceJson returns non-null"), Json.IsValid());
    if (!Json.IsValid())
    {
        return false;
    }

    TestEqual(TEXT("assetKind"), Json->GetStringField(TEXT("assetKind")), FString(TEXT("AnimSequence")));
    TestEqual(TEXT("path"), Json->GetStringField(TEXT("path")), Sequence->GetPathName());

    TestTrue(TEXT("lengthSeconds key present"), Json->HasField(TEXT("lengthSeconds")));
    TestTrue(TEXT("frameRate key present"), Json->HasField(TEXT("frameRate")));
    TestTrue(TEXT("numFrames key present"), Json->HasField(TEXT("numFrames")));
    TestTrue(TEXT("loop key present"), Json->HasField(TEXT("loop")));
    TestTrue(TEXT("interpolation key present"), Json->HasField(TEXT("interpolation")));
    TestTrue(TEXT("additiveType key present"), Json->HasField(TEXT("additiveType")));

    const TArray<TSharedPtr<FJsonValue>>* Notifies = nullptr;
    TestTrue(TEXT("notifies array exists"), Json->TryGetArrayField(TEXT("notifies"), Notifies));
    TestTrue(TEXT("notifies array has length >= 1"), Notifies && Notifies->Num() >= 1);

    if (Notifies && Notifies->Num() >= 1)
    {
        TSharedPtr<FJsonObject> First = (*Notifies)[0].IsValid() ? (*Notifies)[0]->AsObject() : nullptr;
        TestTrue(TEXT("notifies[0] is object"), First.IsValid());
        if (First.IsValid())
        {
            TestEqual(TEXT("notifies[0].name matches NotifyName"),
                First->GetStringField(TEXT("name")), FString(TEXT("TestNotify")));

            bool bBranchingPoint = true;
            TestTrue(TEXT("notifies[0].branchingPoint field exists"),
                First->TryGetBoolField(TEXT("branchingPoint"), bBranchingPoint));
            TestFalse(TEXT("notifies[0].branchingPoint is false"), bBranchingPoint);
        }
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAnimSequenceAssetDumpWritesAnimSequenceAspectFileTest,
    "PinWright.Assets.AnimSequence.AssetDump.WritesAnimSequenceAspectFile",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAnimSequenceAssetDumpWritesAnimSequenceAspectFileTest::RunTest(const FString& Parameters)
{
    FString ObjectPath;
    UAnimSequence* Sequence = NewTransientAnimSequence(ObjectPath);
    FScopedAnimAssetRoot SequenceRoot(Sequence);
    TestNotNull(TEXT("Transient UAnimSequence created"), Sequence);
    if (!Sequence)
    {
        return false;
    }

    const FString ScratchRoot = FPaths::ProjectIntermediateDir()
        / TEXT("AnimSequenceDumpBuilderTests") / FGuid::NewGuid().ToString(EGuidFormats::Digits);

    const AssetDumpHandler::FDumpSingleResult Result =
        AssetDumpHandler::DumpSingleAsset(ObjectPath, ScratchRoot, /*bDiff=*/false);

    TestTrue(TEXT("asset.dump succeeds for transient AnimSequence"), Result.ErrorCode.IsEmpty());
    TestTrue(TEXT("properties.json remains present"), HasDumpFile(Result.WrittenPaths, DumpFileNames::Properties));
    TestTrue(TEXT("anim_sequence.json is written"), HasDumpFile(Result.WrittenPaths, DumpFileNames::AnimSequence));

    const FString AspectPath = FindDumpFile(Result.WrittenPaths, DumpFileNames::AnimSequence);
    TSharedPtr<FJsonObject> Json = LoadJsonFile(AspectPath);
    TestTrue(TEXT("anim_sequence.json parses"), Json.IsValid());
    if (Json.IsValid())
    {
        TestEqual(TEXT("anim_sequence.json assetKind"),
            Json->GetStringField(TEXT("assetKind")), FString(TEXT("AnimSequence")));
        TestTrue(TEXT("lengthSeconds key present"), Json->HasField(TEXT("lengthSeconds")));
        TestTrue(TEXT("frameRate key present"), Json->HasField(TEXT("frameRate")));
        TestTrue(TEXT("numFrames key present"), Json->HasField(TEXT("numFrames")));
        TestTrue(TEXT("loop key present"), Json->HasField(TEXT("loop")));
        TestTrue(TEXT("interpolation key present"), Json->HasField(TEXT("interpolation")));
        TestTrue(TEXT("additiveType key present"), Json->HasField(TEXT("additiveType")));
        TestTrue(TEXT("notifies array exists"), Json->HasField(TEXT("notifies")));
        TestTrue(TEXT("curves array exists"), Json->HasField(TEXT("curves")));
        TestTrue(TEXT("syncMarkers array exists"), Json->HasField(TEXT("syncMarkers")));

        const TArray<TSharedPtr<FJsonValue>>* Notifies = nullptr;
        TestTrue(TEXT("anim_sequence.json notifies array parses"),
            Json->TryGetArrayField(TEXT("notifies"), Notifies));
        TestTrue(TEXT("anim_sequence.json notifies array has notify"),
            Notifies && Notifies->Num() >= 1);
        if (Notifies && Notifies->Num() >= 1)
        {
            TSharedPtr<FJsonObject> First = (*Notifies)[0].IsValid() ? (*Notifies)[0]->AsObject() : nullptr;
            TestTrue(TEXT("anim_sequence.json notifies[0] is object"), First.IsValid());
            if (First.IsValid())
            {
                bool bBranchingPoint = true;
                TestTrue(TEXT("anim_sequence.json notifies[0].branchingPoint field exists"),
                    First->TryGetBoolField(TEXT("branchingPoint"), bBranchingPoint));
                TestFalse(TEXT("anim_sequence.json notifies[0].branchingPoint is false"), bBranchingPoint);
            }
        }
    }

    IFileManager::Get().DeleteDirectory(*ScratchRoot, /*RequireExists=*/false, /*Tree=*/true);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAnimSequenceDumpSyncMarkerDiffTest,
    "PinWright.Assets.AnimSequence.DumpBuilder.SyncMarkerMoveAppearsInDiff",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAnimSequenceDumpSyncMarkerDiffTest::RunTest(const FString& Parameters)
{
    FString SkeletonPackagePath;
    FString SequencePackagePath;
    USkeleton* Skeleton = NewTransientSkeletonWithBones({ FName(TEXT("root")) });
    FScopedAnimAssetRoot SkeletonRoot(Skeleton);
    TestNotNull(TEXT("Synthetic skeleton for sync-marker diff created"), Skeleton);
    if (!Skeleton)
    {
        return false;
    }
    SkeletonPackagePath = Skeleton->GetOutermost()->GetName();

    UAnimSequence* Sequence = nullptr;
    const FString SequenceAssetName = MakeUniqueAnimSeqTestAssetName(
        TEXT("AS_AnimSequenceDumpDiff"));
    SequencePackagePath = FString::Printf(
        TEXT("/Game/PinWrightTests/%s"), *SequenceAssetName);
    UPackage* SequencePackage = CreatePackage(*SequencePackagePath);
    if (SequencePackage)
    {
        Sequence = NewObject<UAnimSequence>(
            SequencePackage, *SequenceAssetName, RF_Public | RF_Standalone);
    }
    TestNotNull(TEXT("Saved UAnimSequence for sync-marker diff created"), Sequence);
    if (!Sequence)
    {
        SkeletonRoot.Reset();
        CleanupTestAsset(SkeletonPackagePath);
        return false;
    }
    Sequence->SetSkeleton(Skeleton);
    Sequence->AddToRoot();
    const FString ObjectPath = Sequence->GetPathName();

    ON_SCOPE_EXIT
    {
        Sequence->RemoveFromRoot();
        CleanupTestAsset(SequencePackagePath);
        SkeletonRoot.Reset();
        CleanupTestAsset(SkeletonPackagePath);
    };

    auto SaveAssetPackage = [this](UObject* Asset, const FString& PackagePath,
        const TCHAR* Description) -> bool
    {
        if (!Asset)
        {
            return false;
        }

        UPackage* Package = Asset->GetOutermost();
        if (!Package)
        {
            return false;
        }

        Asset->ClearFlags(RF_Transient);
        Package->ClearFlags(RF_Transient);
        Package->MarkPackageDirty();

        const FString PackageFilename = FPackageName::LongPackageNameToFilename(
            PackagePath, FPackageName::GetAssetPackageExtension());
        FSavePackageArgs SaveArgs;
        SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
        SaveArgs.SaveFlags = SAVE_NoError;
        const bool bSaved = UPackage::SavePackage(
            Package, Asset, *PackageFilename, SaveArgs);
        TestTrue(*FString::Printf(TEXT("%s saved to disk"), Description), bSaved);
        return bSaved;
    };

    FAssetRegistryModule::AssetCreated(Skeleton);
    if (!SaveAssetPackage(Skeleton, SkeletonPackagePath, TEXT("Synthetic skeleton")))
    {
        return false;
    }

    IAnimationDataController& Controller = Sequence->GetController();
    Controller.InitializeModel();
    Controller.SetFrameRate(FFrameRate(30, 1));
    Controller.SetNumberOfFrames(FFrameNumber(2));
    Controller.NotifyPopulated();

    TArray<FPwSyncMarkerSpec> BeforeMarkers;
    FPwSyncMarkerSpec& Before = BeforeMarkers.AddDefaulted_GetRef();
    Before.MarkerName = FName(TEXT("R"));
    Before.Time = 1.0f / 30.0f;
    const FAnimSequenceSyncMarkerWriteResult BeforeWrite = SetAnimSequenceSyncMarkers(
        Sequence, TArrayView<const FPwSyncMarkerSpec>(BeforeMarkers));
    TestTrue(TEXT("initial marker write succeeds"), BeforeWrite.bSuccess);

    FAssetRegistryModule::AssetCreated(Sequence);
    if (!SaveAssetPackage(Sequence, SequencePackagePath, TEXT("Animation sequence")))
    {
        return false;
    }

    FAssetRegistryModule& AssetRegistryModule =
        FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
    IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();
    TArray<FString> SavedPackageFiles;
    SavedPackageFiles.Add(FPackageName::LongPackageNameToFilename(
        SkeletonPackagePath, FPackageName::GetAssetPackageExtension()));
    SavedPackageFiles.Add(FPackageName::LongPackageNameToFilename(
        SequencePackagePath, FPackageName::GetAssetPackageExtension()));
    AssetRegistry.ScanFilesSynchronous(SavedPackageFiles, /*bForceRescan=*/true);

    const FString ScratchRoot = FPaths::ProjectIntermediateDir()
        / TEXT("AnimSequenceSyncMarkerDiffTests") / FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const AssetDumpHandler::FDumpSingleResult Baseline =
        AssetDumpHandler::DumpSingleAsset(ObjectPath, ScratchRoot, /*bDiff=*/false);
    TestTrue(TEXT("marker baseline dump succeeds"), Baseline.ErrorCode.IsEmpty());
    TestTrue(TEXT("marker baseline writes anim_sequence.json"),
        HasDumpFile(Baseline.WrittenPaths, DumpFileNames::AnimSequence));
    TestTrue(TEXT("marker baseline writes a dump cache record"),
        IFileManager::Get().FileExists(*(Baseline.DumpDir / TEXT(".dumpcache.json"))));

    TArray<FPwSyncMarkerSpec> AfterMarkers;
    FPwSyncMarkerSpec& After = AfterMarkers.AddDefaulted_GetRef();
    After.MarkerName = FName(TEXT("R"));
    After.Time = 2.0f / 30.0f;
    const FAnimSequenceSyncMarkerWriteResult AfterWrite = SetAnimSequenceSyncMarkers(
        Sequence, TArrayView<const FPwSyncMarkerSpec>(AfterMarkers));
    TestTrue(TEXT("moved marker write succeeds"), AfterWrite.bSuccess);

    const AssetDumpHandler::FDumpSingleResult Diff =
        AssetDumpHandler::DumpSingleAsset(ObjectPath, ScratchRoot, /*bDiff=*/true);
    TestTrue(TEXT("marker move diff dump succeeds"), Diff.ErrorCode.IsEmpty());
    TestEqual(TEXT("marker move uses diff mode"), Diff.Mode, FString(TEXT("diff")));

    const FString DiffPath = Diff.DumpDir / TEXT("anim_sequence_diff.txt");
    TestTrue(TEXT("marker move writes the anim_sequence diff sidecar"),
        IFileManager::Get().FileExists(*DiffPath));
    FString DiffText;
    if (IFileManager::Get().FileExists(*DiffPath))
    {
        FFileHelper::LoadFileToString(DiffText, *DiffPath);
    }
    const auto DiffContainsChangedTime = [&DiffText](const FString& Prefix, float Time)
    {
        TArray<FString> DiffLines;
        DiffText.ParseIntoArrayLines(DiffLines, /*bCullEmpty=*/true);
        for (const FString& Line : DiffLines)
        {
            const int32 TimeFieldOffset = Line.Find(TEXT("\"time\":"));
            if (Line.StartsWith(Prefix) && TimeFieldOffset != INDEX_NONE)
            {
                const FString SerializedTime = Line.Mid(TimeFieldOffset + 7).TrimStartAndEnd();
                if (FMath::IsNearlyEqual(
                    static_cast<float>(FCString::Atod(*SerializedTime)), Time))
                {
                    return true;
                }
            }
        }
        return false;
    };
    const bool bHasRemovedMarkerTime = DiffContainsChangedTime(TEXT("-"), Before.Time);
    const bool bHasAddedMarkerTime = DiffContainsChangedTime(TEXT("+"), After.Time);
    TestTrue(TEXT("anim_sequence diff names the changed sync marker"),
        DiffText.Contains(TEXT("\"name\": \"R\""))
        && bHasRemovedMarkerTime
        && bHasAddedMarkerTime);

    const FString NewPath = FindDumpFile(
        Diff.WrittenPaths, TEXT("anim_sequence_new.json"));
    const TSharedPtr<FJsonObject> NewJson = LoadJsonFile(NewPath);
    const TArray<TSharedPtr<FJsonValue>>* NewMarkers = nullptr;
    TestTrue(TEXT("anim_sequence diff writes the new marker sidecar"), NewJson.IsValid());
    TestTrue(TEXT("new anim_sequence sidecar has syncMarkers"),
        NewJson.IsValid() && NewJson->TryGetArrayField(TEXT("syncMarkers"), NewMarkers));
    if (NewMarkers && NewMarkers->Num() == 1 && (*NewMarkers)[0].IsValid())
    {
        const TSharedPtr<FJsonObject> MarkerObject = (*NewMarkers)[0]->AsObject();
        FString NewMarkerName;
        double NewMarkerTime = 0.0;
        const bool bHasName = MarkerObject.IsValid()
            && MarkerObject->TryGetStringField(TEXT("name"), NewMarkerName);
        const bool bHasTime = MarkerObject.IsValid()
            && MarkerObject->TryGetNumberField(TEXT("time"), NewMarkerTime);
        TestTrue(TEXT("new marker sidecar retains the marker name"),
            bHasName && NewMarkerName == TEXT("R"));
        TestTrue(TEXT("new marker sidecar records the moved time"),
            bHasTime && FMath::IsNearlyEqual(
                static_cast<float>(NewMarkerTime), 2.0f / 30.0f));
    }

    IFileManager::Get().DeleteDirectory(*Diff.DumpDir, /*RequireExists=*/false, /*Tree=*/true);
    IFileManager::Get().DeleteDirectory(*ScratchRoot, /*RequireExists=*/false, /*Tree=*/true);
    return true;
}

// Regression for E-rpc-animation-bone-track-readback: per-bone-track keys authored via the
// IAnimationDataController write path must surface through the dump builder as
// boneTracks:[{boneName,keyCount}], not only the aggregate rawTrackCount. Before the fix,
// BuildAnimSequenceJson emitted no boneTracks section, so a hand-keyed bone had no structured
// live readback. Authors the tracks through the controller directly (AddBoneCurve +
// SetBoneTrackKeys with single-element key arrays).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAnimSequenceDumpBuilderBoneTracksReadbackTest,
    "PinWright.Assets.AnimSequence.DumpBuilder.BoneTracksReadback",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAnimSequenceDumpBuilderBoneTracksReadbackTest::RunTest(const FString& Parameters)
{
#if UE_VERSION_OLDER_THAN(5, 5, 0)
    // SKIP on UE 5.3/5.4: authoring bone tracks via IAnimationDataController and closing the
    // bracket triggers UAnimSequence's synchronous DDC recompression (OnModelModified ->
    // BeginCacheDerivedDataForCurrentPlatform). On 5.3 and 5.4 the engine's anim-compression
    // DDC build (AnimationData -> ControlRig/RigVM evaluation, run on a DDC "Foreground Worker"
    // thread) indexes a 2-element array out of bounds and hard-asserts ("Array index out of
    // bounds: 2 into an array of size 2") on this minimal transient 2-bone skeleton — an
    // engine-side crash. It was originally assumed 5.4 guarded it, but the 5.4 host reproduces
    // the identical fatal assert (the crash callstack is entirely engine: ControlRig -> RigVM ->
    // AnimationData -> Engine -> DerivedDataCache, with no plugin frame), so the skip extends
    // through 5.4. The production code under test (BuildBoneTracksArrayJson) reads only the raw
    // data model and is version-agnostic; the crash is in engine compression, not the plugin.
    // Skipping here rather than silently passing keeps the limitation visible.
    PinWrightTestSkip::SkipAssertions(*this, TEXT("engine-version-unsupported"),
        TEXT("Skipping BoneTracksReadback on UE 5.3/5.4: engine anim compression asserts when "
            "re-compressing a hand-authored transient AnimSequence on a minimal skeleton (works on 5.5+)."));
    return true;
#else
    FString ObjectPath;
    UAnimSequence* Sequence = NewTransientAnimSequence(ObjectPath);
    FScopedAnimAssetRoot SequenceRoot(Sequence);
    TestNotNull(TEXT("Transient UAnimSequence created"), Sequence);
    if (!Sequence)
    {
        return false;
    }
    // AddBoneCurve validates each bone against the sequence's skeleton's reference skeleton, so the
    // bones we key MUST exist on a bound skeleton or the add silently no-ops (GetNumBoneTracks==0).
    // pelvis is the root; spine_1 is its child — exactly the two bones the readback expects.
    const FName PelvisBone(TEXT("pelvis"));
    const FName SpineBone(TEXT("spine_1"));
    USkeleton* Skeleton = NewTransientSkeletonWithBones({ PelvisBone, SpineBone });
    FScopedAnimAssetRoot SkeletonRoot(Skeleton);
    TestNotNull(TEXT("Transient skeleton with pelvis/spine_1 created"), Skeleton);
    if (!Skeleton)
    {
        return false;
    }
    Sequence->SetSkeleton(Skeleton);

    // Drive the data-model write path: initialize the model, open a bracket, add the bone track,
    // then set a single transform key on it (single-element arrays => one key per bone).
    {
        IAnimationDataController& Controller = Sequence->GetController();
        Controller.OpenBracket(FText::FromString(TEXT("PinWright BoneTracks Readback Test")), /*bShouldTransact=*/false);
        Controller.InitializeModel();
        Controller.SetNumberOfFrames(FFrameNumber(30), /*bShouldTransact=*/false);

        Controller.AddBoneCurve(PelvisBone, /*bShouldTransact=*/false);
        Controller.SetBoneTrackKeys(PelvisBone, { FVector(1.0, 2.0, 3.0) }, { FQuat::Identity }, { FVector::OneVector }, /*bShouldTransact=*/false);

        Controller.AddBoneCurve(SpineBone, /*bShouldTransact=*/false);
        Controller.SetBoneTrackKeys(SpineBone, { FVector::ZeroVector }, { FQuat::MakeFromEuler(FVector(0, 0, 30)) }, { FVector::OneVector }, /*bShouldTransact=*/false);

        Controller.CloseBracket(/*bShouldTransact=*/false);
    }

    // Sanity: the aggregate count the OLD code exposed sees both tracks.
    if (const IAnimationDataModel* DataModel = Sequence->GetDataModel())
    {
        TestEqual(TEXT("GetNumBoneTracks sees both authored bone tracks"), DataModel->GetNumBoneTracks(), 2);
    }

    // The new structured readback: boneTracks[] carries one {boneName,keyCount} per keyed bone.
    const TArray<TSharedPtr<FJsonValue>> BoneTracks = AnimSequenceDumpBuilder::BuildBoneTracksArrayJson(Sequence);
    TestEqual(TEXT("BuildBoneTracksArrayJson returns one entry per authored bone track"), BoneTracks.Num(), 2);

    TMap<FString, int32> KeyCountByBone;
    for (const TSharedPtr<FJsonValue>& Value : BoneTracks)
    {
        const TSharedPtr<FJsonObject> Obj = Value.IsValid() ? Value->AsObject() : nullptr;
        TestTrue(TEXT("boneTracks[] entry is an object"), Obj.IsValid());
        if (Obj.IsValid())
        {
            FString BoneName;
            int32 KeyCount = -1;
            TestTrue(TEXT("entry has boneName"), Obj->TryGetStringField(TEXT("boneName"), BoneName));
            TestTrue(TEXT("entry has keyCount"), Obj->TryGetNumberField(TEXT("keyCount"), KeyCount));
            KeyCountByBone.Add(BoneName, KeyCount);
        }
    }

    TestTrue(TEXT("pelvis bone track surfaced"), KeyCountByBone.Contains(TEXT("pelvis")));
    TestTrue(TEXT("spine_1 bone track surfaced"), KeyCountByBone.Contains(TEXT("spine_1")));
    if (const int32* PelvisKeys = KeyCountByBone.Find(TEXT("pelvis")))
    {
        TestTrue(TEXT("pelvis keyCount >= 1"), *PelvisKeys >= 1);
    }

    // boneTracks[] must also flow through the full BuildAnimSequenceJson shape (describe_sequence /
    // anim_sequence.json sidecar) alongside the existing rawTrackCount aggregate.
    const TSharedPtr<FJsonObject> Json = AnimSequenceDumpBuilder::BuildAnimSequenceJson(Sequence);
    TestTrue(TEXT("BuildAnimSequenceJson returns non-null"), Json.IsValid());
    if (Json.IsValid())
    {
        const TArray<TSharedPtr<FJsonValue>>* JsonBoneTracks = nullptr;
        TestTrue(TEXT("anim sequence json has boneTracks array"), Json->TryGetArrayField(TEXT("boneTracks"), JsonBoneTracks));
        TestTrue(TEXT("boneTracks array has both authored bones"), JsonBoneTracks && JsonBoneTracks->Num() == 2);

        int32 RawTrackCount = 0;
        TestTrue(TEXT("rawTrackCount aggregate retained"), Json->TryGetNumberField(TEXT("rawTrackCount"), RawTrackCount));
        TestEqual(TEXT("rawTrackCount matches authored track count"), RawTrackCount, 2);
    }

    return true;
#endif
}
