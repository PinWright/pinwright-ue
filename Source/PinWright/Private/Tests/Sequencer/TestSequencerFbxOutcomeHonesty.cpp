// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Tests/Sequencer/SequencerTestFixtures.h"
#include "Tests/TestUtils.h"
#include "Tests/TestWorldUtils.h"
#include "UObject/StrongObjectPtr.h"
#include "Utils/AssetUtils.h"

#include "Channels/MovieSceneDoubleChannel.h"
#include "Channels/MovieSceneChannelProxy.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMeshActor.h"
#include "LevelSequence.h"
#include "MovieScene.h"
#include "Sections/MovieScene3DTransformSection.h"
#include "Tracks/MovieScene3DTransformTrack.h"

namespace SequencerFbxOutcomeHonestyTests
{
    struct FFixture
    {
        TStrongObjectPtr<ULevelSequence> Sequence;
        FString ObjectPath;
        UMovieScene* MovieScene = nullptr;
        FGuid BindingGuid;
        UMovieScene3DTransformTrack* Track = nullptr;
        UMovieScene3DTransformSection* Section = nullptr;

        // The sequence is RF_Standalone, so releasing the strong pointer is not enough to reclaim
        // it; detach it from its /Engine/Transient package as well.
        ~FFixture()
        {
            Sequence.Reset();
            CleanupTestAsset(ObjectPath);
        }
    };

    inline void SetBinding(const TSharedPtr<FJsonObject>& Payload, const FGuid& BindingGuid)
    {
        TArray<TSharedPtr<FJsonValue>> Bindings;
        Bindings.Add(MakeShared<FJsonValueString>(
            BindingGuid.ToString(EGuidFormats::Digits)));
        Payload->SetArrayField(TEXT("bindings"), Bindings);
    }

    inline bool BuildFixture(FAutomationTestBase& Test, const TCHAR* Prefix, FFixture& Out)
    {
        ULevelSequence* Sequence = SequencerTestFixtures::MakeTransientSequence(Prefix, Out.ObjectPath);
        if (!Test.TestNotNull(TEXT("transient LevelSequence created"), Sequence))
        {
            return false;
        }
        Out.Sequence.Reset(Sequence);
        Out.MovieScene = Sequence->GetMovieScene();
        if (!Test.TestNotNull(TEXT("transient sequence has a MovieScene"), Out.MovieScene))
        {
            return false;
        }

        const FString ActorLabel = FString::Printf(TEXT("PW_FbxOutcome_%s"),
            *FGuid::NewGuid().ToString(EGuidFormats::Digits));
        AStaticMeshActor* Actor = SpawnActorInActiveWorld<AStaticMeshActor>(
            AStaticMeshActor::StaticClass(), FVector::ZeroVector, FRotator::ZeroRotator, ActorLabel);
        if (!Test.TestNotNull(TEXT("FBX fixture actor spawned"), Actor))
        {
            return false;
        }
        Actor->GetStaticMeshComponent()->SetMobility(EComponentMobility::Movable);

        Out.BindingGuid = Out.MovieScene->AddPossessable(ActorLabel, AStaticMeshActor::StaticClass());
        if (!Test.TestTrue(TEXT("FBX fixture binding is valid"), Out.BindingGuid.IsValid()))
        {
            return false;
        }
        Sequence->BindPossessableObject(Out.BindingGuid, *Actor, Actor->GetWorld());

        Out.Track = Out.MovieScene->AddTrack<UMovieScene3DTransformTrack>(Out.BindingGuid);
        Out.Section = Out.Track
            ? Cast<UMovieScene3DTransformSection>(Out.Track->CreateNewSection())
            : nullptr;
        if (!Test.TestNotNull(TEXT("FBX fixture transform track created"), Out.Track)
            || !Test.TestNotNull(TEXT("FBX fixture transform section created"), Out.Section))
        {
            return false;
        }
        Out.Track->AddSection(*Out.Section);

        TArrayView<FMovieSceneDoubleChannel*> Channels =
            Out.Section->GetChannelProxy().GetChannels<FMovieSceneDoubleChannel>();
        if (!Test.TestTrue(TEXT("FBX fixture has a transform channel"),
                Channels.Num() > 0 && Channels[0] != nullptr))
        {
            return false;
        }
        const FFrameNumber Start(0);
        const FFrameNumber End = Out.MovieScene->GetTickResolution().AsFrameNumber(1.0);
        Out.Section->SetRange(TRange<FFrameNumber>(Start, End));
        Out.MovieScene->SetPlaybackRange(TRange<FFrameNumber>(Start, End));
        Channels[0]->GetData().UpdateOrAddKey(Start, FMovieSceneDoubleValue(0.0));
        Channels[0]->GetData().UpdateOrAddKey(End, FMovieSceneDoubleValue(100.0));
        return true;
    }

    inline TSharedPtr<FJsonObject> MakeExportPayload(
        const FFixture& Fixture, const FString& FilePath, bool bOverwrite = false)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("path"), Fixture.ObjectPath);
        Payload->SetStringField(TEXT("filePath"), FilePath);
        Payload->SetBoolField(TEXT("overwrite"), bOverwrite);
        SetBinding(Payload, Fixture.BindingGuid);
        return Payload;
    }

    inline FString MakeOutputPath(const TCHAR* Prefix)
    {
        return FPaths::ConvertRelativePathToFull(
            FPaths::ProjectSavedDir() / TEXT("PinWright") / TEXT("Tests") /
            FString::Printf(TEXT("%s_%s.fbx"), Prefix,
                *FGuid::NewGuid().ToString(EGuidFormats::Digits)));
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSequencerFbxNoOpImportRejectedTest,
    "PinWright.Sequencer.FbxImport.NoOpRejected",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSequencerFbxNoOpImportRejectedTest::RunTest(const FString& Parameters)
{
    using namespace SequencerFbxOutcomeHonestyTests;
    FScopedEditorWorldActorGuard WorldGuard;
    FFixture Fixture;
    if (!BuildFixture(*this, TEXT("FbxNoOpImport"), Fixture))
    {
        return true;
    }

    const FString FbxPath = MakeOutputPath(TEXT("FbxNoOpImport"));
    ON_SCOPE_EXIT
    {
        IFileManager::Get().Delete(
            *FbxPath, /*RequireExists=*/false, /*EvenReadOnly=*/true, /*Quiet=*/true);
    };

    FTestResponseCapture ExportCapture;
    TestTrue(TEXT("fixture FBX export handler invoked"), InvokeHandlerWithCapture(
        TEXT("sequencer.export_fbx"), MakeExportPayload(Fixture, FbxPath), ExportCapture));
    if (!TestTrue(TEXT("fixture FBX export succeeded"),
            ExportCapture.bWasCalled && ExportCapture.bSuccess))
    {
        return true;
    }

    TArrayView<FMovieSceneDoubleChannel*> Channels =
        Fixture.Section->GetChannelProxy().GetChannels<FMovieSceneDoubleChannel>();
    Channels[0]->Reset();
    TestEqual(TEXT("target binding has zero keys before the no-match import"),
        Channels[0]->GetNumKeys(), 0);
    Fixture.MovieScene->SetObjectDisplayName(Fixture.BindingGuid,
        FText::FromString(FString::Printf(TEXT("PW_NoMatch_%s"),
            *FGuid::NewGuid().ToString(EGuidFormats::Digits))));

    TSharedPtr<FJsonObject> ImportPayload = MakeShared<FJsonObject>();
    ImportPayload->SetStringField(TEXT("path"), Fixture.ObjectPath);
    ImportPayload->SetStringField(TEXT("filePath"), FbxPath);
    ImportPayload->SetBoolField(TEXT("matchByNameOnly"), true);
    SetBinding(ImportPayload, Fixture.BindingGuid);

    FTestResponseCapture ImportCapture;
    TestTrue(TEXT("no-match FBX import handler invoked"), InvokeHandlerWithCapture(
        TEXT("sequencer.import_fbx"), ImportPayload, ImportCapture));
    TestTrue(TEXT("no-match FBX import is refused"),
        ImportCapture.bWasCalled && !ImportCapture.bSuccess);
    TestEqual(TEXT("no-match FBX import reports NOTHING_IMPORTED"),
        ImportCapture.ErrorCode, FString(TEXT("NOTHING_IMPORTED")));
    TestNotNull(TEXT("no-match response carries measured before/after counts"),
        ImportCapture.Result.Get());
    if (ImportCapture.Result.IsValid())
    {
        TestEqual(TEXT("no-match import leaves track count unchanged"),
            ImportCapture.Result->GetNumberField(TEXT("tracksBefore")),
            ImportCapture.Result->GetNumberField(TEXT("tracksAfter")));
        TestEqual(TEXT("no-match import leaves key count unchanged"),
            ImportCapture.Result->GetNumberField(TEXT("keysBefore")),
            ImportCapture.Result->GetNumberField(TEXT("keysAfter")));
    }
    TestEqual(TEXT("no-match import leaves the target channel empty"),
        Channels[0]->GetNumKeys(), 0);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSequencerFbxAtomicStaleDestinationTest,
    "PinWright.Sequencer.FbxExport.AtomicStaleDestination",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSequencerFbxAtomicStaleDestinationTest::RunTest(const FString& Parameters)
{
    using namespace SequencerFbxOutcomeHonestyTests;
    FScopedEditorWorldActorGuard WorldGuard;
    FFixture Fixture;
    if (!BuildFixture(*this, TEXT("FbxAtomicExport"), Fixture))
    {
        return true;
    }

    const FString FbxPath = MakeOutputPath(TEXT("FbxAtomicExport"));
    const FString StagingProofPath = MakeOutputPath(TEXT("FbxAtomicExportStageProof"));
    const ANSICHAR StaleContents[] = "PINWRIGHT_STALE_FBX_SENTINEL";
    TArray<uint8> OriginalBytes;
    OriginalBytes.Append(reinterpret_cast<const uint8*>(StaleContents),
        UE_ARRAY_COUNT(StaleContents) - 1);
    TestNotNull(TEXT("export_fbx declares its overwrite wire parameter"),
        GetRegisteredParamSpec(TEXT("sequencer.export_fbx"), TEXT("overwrite")));
    TestTrue(TEXT("stale destination parent directory created"),
        IFileManager::Get().MakeDirectory(*FPaths::GetPath(FbxPath), /*Tree=*/true));
    TestTrue(TEXT("stale destination fixture written"),
        FFileHelper::SaveArrayToFile(OriginalBytes, *FbxPath));
    ON_SCOPE_EXIT
    {
        FPlatformFileManager::Get().GetPlatformFile().SetReadOnly(*FbxPath, false);
        IFileManager::Get().Delete(
            *FbxPath, /*RequireExists=*/false, /*EvenReadOnly=*/true, /*Quiet=*/true);
        IFileManager::Get().Delete(
            *StagingProofPath, /*RequireExists=*/false, /*EvenReadOnly=*/true, /*Quiet=*/true);
    };

    FTestResponseCapture RefuseCapture;
    InvokeHandlerWithCapture(TEXT("sequencer.export_fbx"),
        MakeExportPayload(Fixture, FbxPath), RefuseCapture);
    TestTrue(TEXT("existing FBX is refused unless overwrite is explicit"),
        RefuseCapture.bWasCalled && !RefuseCapture.bSuccess);
    TestEqual(TEXT("existing FBX refusal uses ALREADY_EXISTS"),
        RefuseCapture.ErrorCode, FString(TEXT("ALREADY_EXISTS")));

    TArray<uint8> AfterRefusal;
    TestTrue(TEXT("stale destination remains readable after default refusal"),
        FFileHelper::LoadFileToArray(AfterRefusal, *FbxPath));
    TestTrue(TEXT("default refusal preserves stale destination bytes"),
        AfterRefusal == OriginalBytes);

    // Prove this fixture can produce fresh FBX bytes before inducing the destination-only
    // failure below. Without this control, EXPORT_FAILED could come from the engine staging
    // nothing and the test would never exercise AtomicFileWriter's replacement path.
    FTestResponseCapture StagingProofCapture;
    InvokeHandlerWithCapture(TEXT("sequencer.export_fbx"),
        MakeExportPayload(Fixture, StagingProofPath), StagingProofCapture);
    const int64 StagingProofSize = IFileManager::Get().FileSize(*StagingProofPath);
    TArray<uint8> StagingProofBytes;
    const bool bStagingProofReadable =
        FFileHelper::LoadFileToArray(StagingProofBytes, *StagingProofPath);
    const bool bStagingProofSucceeded = StagingProofCapture.bWasCalled
        && StagingProofCapture.bSuccess && StagingProofSize > 0
        && bStagingProofReadable && StagingProofBytes.Num() == StagingProofSize
        && StagingProofBytes != OriginalBytes;
    TestTrue(TEXT("control export succeeds before publication failure is forced"),
        StagingProofCapture.bWasCalled && StagingProofCapture.bSuccess);
    TestTrue(TEXT("control export leaves a readable non-empty staged FBX"),
        StagingProofSize > 0 && bStagingProofReadable
            && StagingProofBytes.Num() == StagingProofSize);
    TestTrue(TEXT("fresh staged FBX bytes differ from the stale destination"),
        StagingProofBytes != OriginalBytes);
    if (!bStagingProofSucceeded)
    {
        // Assertions above deliberately fail the test. Do not turn a broken exporter into a
        // skip, and do not continue to a publication check whose precondition is unproven.
        return true;
    }

    IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
    if (!TestTrue(TEXT("destination made read-only to force atomic publish failure"),
            PlatformFile.SetReadOnly(*FbxPath, true)))
    {
        return true;
    }

    FTestResponseCapture ReplaceCapture;
    InvokeHandlerWithCapture(TEXT("sequencer.export_fbx"),
        MakeExportPayload(Fixture, FbxPath, /*bOverwrite=*/true), ReplaceCapture);
    TestTrue(TEXT("failed atomic replacement is reported as an error"),
        ReplaceCapture.bWasCalled && !ReplaceCapture.bSuccess);
    TestEqual(TEXT("failed atomic replacement uses EXPORT_FAILED"),
        ReplaceCapture.ErrorCode, FString(TEXT("EXPORT_FAILED")));
    TestTrue(TEXT("failure came from atomic publication after staging succeeded"),
        ReplaceCapture.Message.Contains(TEXT("Failed to publish FBX")));

    TArray<uint8> AfterFailedReplace;
    TestTrue(TEXT("destination remains readable after failed atomic publication"),
        FFileHelper::LoadFileToArray(AfterFailedReplace, *FbxPath));
    TestTrue(TEXT("failed atomic replacement preserves the original bytes exactly"),
        AfterFailedReplace == OriginalBytes);

    TArray<FString> LeakedStageFiles;
    IFileManager::Get().FindFiles(LeakedStageFiles, *(FbxPath + TEXT(".*.stage.fbx")),
        /*Files=*/true, /*Directories=*/false);
    TestEqual(TEXT("failed export cleans its task-owned FBX staging file"),
        LeakedStageFiles.Num(), 0);
    return true;
}
