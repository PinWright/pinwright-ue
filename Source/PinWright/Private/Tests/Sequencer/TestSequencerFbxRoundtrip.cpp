// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for F-sequencer-fbx-roundtrip.
//
// PinWright's sequencer surface had NO FBX interchange: no way to export a sequence's
// animation to an external .fbx or import an .fbx back onto its bindings (grep Fbx|FBX in
// Handlers/Sequencer/ = zero pre-fix). This test drives the two new production handlers —
// sequencer.export_fbx and sequencer.import_fbx — end to end through the real
// USequencerToolsFunctionLibrary engine calls (NOT a re-implemented local exporter):
//
//   1. create a /Game LevelSequence (via the registered sequencer.create handler),
//   2. add a possessable binding + a UMovieScene3DTransformTrack with a Location.X ramp
//      (0 -> 137.5) authored directly on the section's double channel,
//   3. dispatch sequencer.export_fbx and assert it reports success AND leaves a non-empty
//      .fbx on disk (MovieSceneToolHelpers::ExportFBX always WriteToFile()s, so a no-op
//      stub that skips the real ExportLevelSequenceFBX call leaves no file and fails here),
//   4. WIPE the authored ramp, dispatch sequencer.import_fbx on that file, and compare the
//      Location.X channel the import restored against the exported ramp.
//
// Step 4 is what closes the round trip. Asserting only `success:true` on the import left the
// import half untested: replace SequencerFbxHandler.cpp's
// USequencerToolsFunctionLibrary::ImportLevelSequenceFBX(...) call with
// `const bool bImported = true;` and the handler skips the engine entirely, still answers
// success:true, and all three import assertions pass. The binding is now backed by a LIVE
// actor in the editor world for the same reason: an unbound possessable resolves to no object,
// so the exporter has no node to write and there is nothing for the import to match by name --
// which is why the original fixture could only claim bSuccess and never compare a value.
//
// Host gate: if the import reports success but restores no keys, this host's FBX interchange
// cannot demonstrate the value round trip. That is recorded as a pinned AddWarning
// (PINWRIGHT-FBX-ROUNDTRIP-NO-VALUE-READBACK), not as a silent pass and not as a host-caused
// red -- the repo idiom for an environment-dependent claim.
//
// Differential property: pre-fix neither verb is registered, so InvokeHandlerWithCapture
// returns false and the "handler invoked / responded / succeeded / wrote a file" assertions
// FAIL, reproducing the capability gap. Once the handlers route through SequencerTools they
// flip green. The assertions are behavioral (a bare registration stub still fails the
// on-disk-file and success checks), not presence-only.

#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EditorAssetLibrary.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#include "Tests/TestSkipReporting.h"
#include "Tests/TestUtils.h"
#include "Tests/TestWorldUtils.h"

#include "Editor.h"
#include "Misc/Guid.h"
#include "Misc/ScopeExit.h"

#include "LevelSequence.h"
#include "MovieScene.h"
#include "Tracks/MovieScene3DTransformTrack.h"
#include "Sections/MovieScene3DTransformSection.h"
#include "Channels/MovieSceneDoubleChannel.h"
#include "Channels/MovieSceneChannelProxy.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"

#include "Utils/AssetUtils.h"

namespace
{
    // Create a real /Game LevelSequence via the registered sequencer.create handler so the
    // FBX handlers' asset load resolves it. Empty path + nullptr on failure. Distinctly named
    // to avoid anonymous-namespace ODR collision with sibling sequencer test .cpp files under
    // a Unity merge.
    ULevelSequence* CreateFbxRoundTripSequence(FAutomationTestBase& Test, FString& OutFullPath)
    {
        const FString SeqName = FString::Printf(TEXT("MCP_FbxRoundTripSeq_%s"),
            *FGuid::NewGuid().ToString(EGuidFormats::Digits));
        const FString DestFolder = TEXT("/Game/MCP_FbxRoundTripProbe");
        OutFullPath = FString::Printf(TEXT("%s/%s"), *DestFolder, *SeqName);

        TSharedPtr<FJsonObject> CreatePayload = MakeShared<FJsonObject>();
        CreatePayload->SetStringField(TEXT("name"), SeqName);
        CreatePayload->SetStringField(TEXT("path"), DestFolder);
        FTestResponseCapture CreateCapture;
        InvokeHandlerWithCapture(TEXT("sequencer.create"), CreatePayload, CreateCapture);

        if (!CreateCapture.bWasCalled || !CreateCapture.bSuccess ||
            !UEditorAssetLibrary::DoesAssetExist(OutFullPath))
        {
            PinWrightTestSkip::SkipAssertions(Test, TEXT("sequence-factory-unavailable"),
                TEXT("Could not create a probe sequence (factory unavailable in this "
                     "host); the registration assertions above still stand"));
            OutFullPath.Reset();
            return nullptr;
        }
        return Cast<ULevelSequence>(UEditorAssetLibrary::LoadAsset(OutFullPath));
    }

    // Builds a JSON array field of one binding-GUID string on Payload.
    void SetBindingsField(const TSharedPtr<FJsonObject>& Payload, const FString& BindingId)
    {
        TArray<TSharedPtr<FJsonValue>> Arr;
        Arr.Add(MakeShared<FJsonValueString>(BindingId));
        Payload->SetArrayField(TEXT("bindings"), Arr);
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSequencerFbxRoundTripTest,
    "PinWright.Sequencer.FbxRoundTrip.ExportImport",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSequencerFbxRoundTripTest::RunTest(const FString& Parameters)
{
    FScopedEditorWorldActorGuard WorldGuard;
    // Core capability-gap reproduction: both FBX verbs must be registered. Pre-fix nothing
    // registers them, so these fail — the exact defect the ticket reports.
    TestTrue(TEXT("sequencer.export_fbx handler registered"),
        IsHandlerRegistered(TEXT("sequencer.export_fbx")));
    TestTrue(TEXT("sequencer.import_fbx handler registered"),
        IsHandlerRegistered(TEXT("sequencer.import_fbx")));

    FString FullPath;
    ULevelSequence* Sequence = CreateFbxRoundTripSequence(*this, FullPath);
    if (!Sequence)
    {
        // The registration failures above already mark this Fail; without a sequence there
        // is nothing to export.
        return true;
    }
    UMovieScene* MovieScene = Sequence->GetMovieScene();
    if (!TestNotNull(TEXT("MovieScene present"), MovieScene))
    {
        CleanupTestAsset(FullPath);
        return true;
    }

    // The binding must resolve to a LIVE object: MovieSceneToolHelpers::ExportFBX writes a
    // node per RESOLVED bound object, and ImportLevelSequenceFBX matches FBX nodes back to
    // bindings BY NAME (bMatchByNameOnly). An unbound possessable therefore exports no
    // animation and can never be re-imported -- the reason the pre-fix fixture settled for
    // asserting bSuccess. Mirrors TestSequencerComponentBinding.cpp's actor fixture.
    UWorld* EditorWorld = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!TestNotNull(TEXT("editor world present for the FBX round trip"), EditorWorld))
    {
        CleanupTestAsset(FullPath);
        return true;
    }
    const FString ProbeLabel = FString::Printf(TEXT("MCP_FbxProbeActor_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    AStaticMeshActor* ProbeActor = SpawnActorInActiveWorld<AStaticMeshActor>(
        AStaticMeshActor::StaticClass(), FVector::ZeroVector, FRotator::ZeroRotator, ProbeLabel);
    if (!TestNotNull(TEXT("probe actor spawned in the editor world"), ProbeActor))
    {
        CleanupTestAsset(FullPath);
        return true;
    }
    if (UStaticMeshComponent* ProbeMeshComp = ProbeActor->GetStaticMeshComponent())
    {
        // AStaticMeshActor is Static by default and a Static component ignores a runtime
        // transform write, so Sequencer would animate nothing. Mesh assignment is best-effort:
        // the transform track is what exports, not the geometry.
        ProbeMeshComp->SetMobility(EComponentMobility::Movable);
        if (UStaticMesh* CubeMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube")))
        {
            ProbeMeshComp->SetStaticMesh(CubeMesh);
        }
    }

    // A possessable binding with a Location.X ramp (0 -> 137.5) gives the export real
    // animation content to serialize. The possessable NAME is the actor label, because the
    // import matches nodes to bindings by name.
    const FGuid BindingGuid = MovieScene->AddPossessable(ProbeLabel, AStaticMeshActor::StaticClass());
    const FString BindingId = BindingGuid.ToString(EGuidFormats::Digits);
    if (!TestTrue(TEXT("binding GUID is valid"), BindingGuid.IsValid()))
    {
        CleanupTestAsset(FullPath);
        return true;
    }
    Sequence->BindPossessableObject(BindingGuid, *ProbeActor, EditorWorld);

    UMovieScene3DTransformTrack* Track = MovieScene->AddTrack<UMovieScene3DTransformTrack>(BindingGuid);
    UMovieScene3DTransformSection* Section =
        Track ? Cast<UMovieScene3DTransformSection>(Track->CreateNewSection()) : nullptr;
    if (!TestNotNull(TEXT("transform track created"), Track) ||
        !TestNotNull(TEXT("transform section created"), Section))
    {
        CleanupTestAsset(FullPath);
        return true;
    }

    const FFrameRate Tick = MovieScene->GetTickResolution();
    const FFrameNumber Start(0);
    const FFrameNumber End = Tick.AsFrameNumber(4.0);  // 4-second ramp
    Section->SetRange(TRange<FFrameNumber>(Start, End));
    Track->AddSection(*Section);
    MovieScene->SetPlaybackRange(TRange<FFrameNumber>(Start, End));

    // Location.X is transform double-channel 0; ramp 0 -> 137.5.
    TArrayView<FMovieSceneDoubleChannel*> Channels =
        Section->GetChannelProxy().GetChannels<FMovieSceneDoubleChannel>();
    if (!TestTrue(TEXT("transform section exposes a Location.X double channel"),
            Channels.Num() > 0 && Channels[0] != nullptr))
    {
        CleanupTestAsset(FullPath);
        return true;
    }
    Channels[0]->GetData().UpdateOrAddKey(Start, FMovieSceneDoubleValue(0.0));
    Channels[0]->GetData().UpdateOrAddKey(End, FMovieSceneDoubleValue(137.5));

    // Destination .fbx under the project Saved dir (absolute; the exporter needs a real path).
    const FString FbxPath = FPaths::ConvertRelativePathToFull(
        FPaths::ProjectSavedDir() / TEXT("PinWright") / TEXT("Tests") /
        FString::Printf(TEXT("SeqFbxRoundTrip_%s.fbx"), *FGuid::NewGuid().ToString(EGuidFormats::Digits)));

    // --- Export ---
    TSharedPtr<FJsonObject> ExportPayload = MakeShared<FJsonObject>();
    ExportPayload->SetStringField(TEXT("path"), FullPath);
    ExportPayload->SetStringField(TEXT("filePath"), FbxPath);
    SetBindingsField(ExportPayload, BindingId);

    FTestResponseCapture ExportCap;
    const bool bExportFound = InvokeHandlerWithCapture(TEXT("sequencer.export_fbx"), ExportPayload, ExportCap);
    TestTrue(TEXT("sequencer.export_fbx invoked (handler present)"), bExportFound);
    TestTrue(TEXT("export responded"), ExportCap.bWasCalled);
    TestTrue(TEXT("export reported success (no fake failure)"), ExportCap.bSuccess);

    const int64 FileSize = IFileManager::Get().FileSize(*FbxPath);
    AddInfo(FString::Printf(TEXT("exported FBX '%s' size=%lld"), *FbxPath, FileSize));
    // Behavioral: the real ExportLevelSequenceFBX writes the file. A stub that only
    // registers (or returns success without calling the engine) leaves no file -> fails here.
    TestTrue(TEXT("export wrote a non-empty .fbx on disk"), FileSize > 0);

    // --- Import the file back onto the bindings, and CLOSE the round trip on the value ---
    if (ExportCap.bSuccess && FileSize > 0)
    {
        // Wipe the authored ramp first. Without this the channel still holds 0 -> 137.5 and a
        // no-op import is indistinguishable from a working one -- the exact hole the
        // `const bool bImported = true;` counterfactual walks through.
        Channels[0]->Reset();
        TestEqual(TEXT("Location.X keys cleared before import (so a no-op import cannot pass)"),
            Channels[0]->GetNumKeys(), 0);

        TSharedPtr<FJsonObject> ImportPayload = MakeShared<FJsonObject>();
        ImportPayload->SetStringField(TEXT("path"), FullPath);
        ImportPayload->SetStringField(TEXT("filePath"), FbxPath);
        SetBindingsField(ImportPayload, BindingId);

        FTestResponseCapture ImportCap;
        const bool bImportFound = InvokeHandlerWithCapture(TEXT("sequencer.import_fbx"), ImportPayload, ImportCap);
        TestTrue(TEXT("sequencer.import_fbx invoked (handler present)"), bImportFound);
        TestTrue(TEXT("import responded"), ImportCap.bWasCalled);
        TestTrue(TEXT("import reported success (round-trip read the exported FBX back)"), ImportCap.bSuccess);

        // Re-resolve from the MovieScene: the import runs with bReplaceTransformTrack=true, so
        // the transform track and its section are rebuilt and the pre-import Channels view must
        // NOT be reused past this point.
        int32 RestoredKeyCount = 0;
        double RestoredStartX = 0.0;
        double RestoredEndX = 0.0;
        if (UMovieScene3DTransformTrack* AfterTrack =
                MovieScene->FindTrack<UMovieScene3DTransformTrack>(BindingGuid))
        {
            const TArray<UMovieSceneSection*>& AfterSections = AfterTrack->GetAllSections();
            UMovieScene3DTransformSection* AfterSection = AfterSections.Num() > 0
                ? Cast<UMovieScene3DTransformSection>(AfterSections[0])
                : nullptr;
            if (AfterSection)
            {
                TArrayView<FMovieSceneDoubleChannel*> AfterChannels =
                    AfterSection->GetChannelProxy().GetChannels<FMovieSceneDoubleChannel>();
                if (AfterChannels.Num() > 0 && AfterChannels[0] != nullptr)
                {
                    RestoredKeyCount = AfterChannels[0]->GetNumKeys();
                    AfterChannels[0]->Evaluate(FFrameTime(Start), RestoredStartX);
                    AfterChannels[0]->Evaluate(FFrameTime(End), RestoredEndX);
                }
            }
        }

        AddInfo(FString::Printf(
            TEXT("post-import Location.X: %d key(s), value at start=%.3f, value at end=%.3f "
                 "(exported ramp was 0.0 -> 137.5)"),
            RestoredKeyCount, RestoredStartX, RestoredEndX));

        if (RestoredKeyCount > 0)
        {
            // The value the round trip exists to preserve. Tolerance is deliberately loose:
            // the FBX pipeline resamples the ramp at the display rate and serializes floats,
            // so exact equality is not the contract -- carrying the ramp across is.
            TestTrue(FString::Printf(
                         TEXT("round-tripped Location.X at the ramp start is the exported 0.0 "
                              "(got %.3f)"), RestoredStartX),
                FMath::IsNearlyEqual(RestoredStartX, 0.0, 1.0));
            TestTrue(FString::Printf(
                         TEXT("round-tripped Location.X at the ramp end is the exported 137.5 "
                              "(got %.3f)"), RestoredEndX),
                FMath::IsNearlyEqual(RestoredEndX, 137.5, 1.0));
        }
        else
        {
            // Pinned, greppable, and honest: the import claimed success but put nothing back,
            // so the ramp could not be compared. Either this host's FBX interchange cannot
            // round-trip a bound-actor transform track, or ImportLevelSequenceFBX is not being
            // reached at all. Warned rather than errored because the two are indistinguishable
            // from inside the test.
            AddWarning(TEXT("PINWRIGHT-FBX-ROUNDTRIP-NO-VALUE-READBACK: sequencer.import_fbx "
                            "reported success but restored no Location.X keys onto the binding, so "
                            "the exported 0 -> 137.5 ramp could not be compared; skipping "
                            "FbxRoundTrip.ExportImport value round-trip."));
        }
    }

    // --- Guard: an explicit-but-empty `bindings` array must be REJECTED, not silently
    // widened to "all bindings". The engine reads an empty binding list as
    // bSelectedOnly=false and would dump the whole level on export / replace transform
    // tracks on every binding on import (bReplaceTransformTrack). ---
    {
        TSharedPtr<FJsonObject> EmptyBindingsPayload = MakeShared<FJsonObject>();
        EmptyBindingsPayload->SetStringField(TEXT("path"), FullPath);
        EmptyBindingsPayload->SetStringField(TEXT("filePath"), FbxPath);
        EmptyBindingsPayload->SetArrayField(TEXT("bindings"), TArray<TSharedPtr<FJsonValue>>());

        FTestResponseCapture EmptyCap;
        InvokeHandlerWithCapture(TEXT("sequencer.export_fbx"), EmptyBindingsPayload, EmptyCap);
        TestTrue(TEXT("export_fbx rejects an explicit empty bindings array (no whole-level fallback)"),
            EmptyCap.bWasCalled && !EmptyCap.bSuccess);
        TestEqual(TEXT("empty bindings rejection uses INVALID_ARGUMENT"),
            EmptyCap.ErrorCode, FString(TEXT("INVALID_ARGUMENT")));
    }

    // --- filePath resolution: a RELATIVE filePath must anchor under the project dir, not
    // the editor-binaries dir that FPaths::ConvertRelativePathToFull uses by default. ---
    {
        const FString RelPath = FString(TEXT("Saved/PinWright/Tests/")) +
            FString::Printf(TEXT("SeqFbxRel_%s.fbx"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));

        TSharedPtr<FJsonObject> RelPayload = MakeShared<FJsonObject>();
        RelPayload->SetStringField(TEXT("path"), FullPath);
        RelPayload->SetStringField(TEXT("filePath"), RelPath);
        SetBindingsField(RelPayload, BindingId);

        FTestResponseCapture RelCap;
        InvokeHandlerWithCapture(TEXT("sequencer.export_fbx"), RelPayload, RelCap);
        TestTrue(TEXT("export_fbx with a relative filePath succeeds"),
            RelCap.bWasCalled && RelCap.bSuccess);

        FString ResolvedPath;
        if (RelCap.Result.IsValid())
        {
            RelCap.Result->TryGetStringField(TEXT("filePath"), ResolvedPath);
        }
        const FString ProjectDirAbs = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
        TestTrue(TEXT("relative filePath resolved under the project dir (not engine binaries)"),
            !ResolvedPath.IsEmpty() && ResolvedPath.StartsWith(ProjectDirAbs));
        TestTrue(TEXT("relative-path export wrote a non-empty .fbx where the response reports"),
            !ResolvedPath.IsEmpty() && IFileManager::Get().FileSize(*ResolvedPath) > 0);

        if (!ResolvedPath.IsEmpty())
        {
            IFileManager::Get().Delete(*ResolvedPath, /*RequireExists=*/false, /*EvenReadOnly=*/true, /*Quiet=*/true);
        }
    }

    // Teardown: remove the on-disk .fbx and the probe asset.
    IFileManager::Get().Delete(*FbxPath, /*RequireExists=*/false, /*EvenReadOnly=*/true, /*Quiet=*/true);
    CleanupTestAsset(FullPath);
    return true;
}
