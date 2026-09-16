// Copyright (c) 2026 Alexander Penkin. MIT License.

// Red test for B-sequencer-section-range-display-frames-as-ticks.
//
// sequencer.add_camera_track (and its siblings add_animation_track / add_audio_track)
// take a duration in SECONDS and author a UMovieSceneSection, but they size the
// section by converting the seconds to DISPLAY-RATE frames
// (FFrameRate::AsFrameTime(Seconds)) and handing those frame numbers straight to
// UMovieSceneSection::SetRange() — which stores frame numbers in the MovieScene's
// TICK RESOLUTION, not its DisplayRate. At the default 24 fps DisplayRate / 24000
// TickResolution the two units differ by a factor of 1000, so a "5 second" camera
// cut is authored spanning 120 ticks (0.005 s) instead of 120000 ticks (5 s) —
// ~1000x too short — while the RPC still reports success:true.
//
// This test drives the PRODUCTION sequencer.add_camera_track handler through the
// real registration list (InvokeHandlerWithCapture), then inspects the resulting
// UMovieSceneCameraCutSection's stored range and asserts the CORRECT behavior: the
// section must span the requested number of seconds when its tick-resolution frame
// range is converted back to seconds. It re-derives the expected range from the
// MovieScene's own GetTickResolution() (the spec — "seconds * TickResolution ticks"),
// not from a re-implementation of the handler's conversion.
//
// Fixture is content-free: the camera is spawned in the editor world exactly as
// sequencer.add_camera does (SpawnActorInActiveWorld<ACameraActor> +
// Spawned->GetPathName()), which is the loadable cameraActorPath form add_camera_track
// resolves via LoadObject<ACameraActor>.
//
// Differential property: pre-fix SetRange receives DisplayRate frames (120), so the
// section spans 0.005 s and both the tick-equality and the ~5 s duration assertions
// FAIL, reproducing the defect. Once the handler converts seconds to tick-resolution
// frames (e.g. MovieScene->GetTickResolution().AsFrameNumber(Seconds)), the section
// spans 120000 ticks / 5 s and the assertions flip green. success:true is asserted as
// a precondition (the handler reports success both pre- and post-fix — it documents the
// false-success context); the section-range assertions are what distinguish fixed from
// broken.
#include "Misc/AutomationTest.h"

#include "Dom/JsonObject.h"
#include "EditorAssetLibrary.h"
#include "Editor.h"
#include "Misc/Guid.h"
#include "Misc/ScopeExit.h"

#include "LevelSequence.h"
#include "MovieScene.h"
#include "Tracks/MovieSceneCameraCutTrack.h"
#include "Sections/MovieSceneCameraCutSection.h"
#include "Camera/CameraActor.h"
#include "Engine/World.h"

#include "Utils/AssetUtils.h"
#include "Tests/TestUtils.h"
#include "Tests/TestWorldUtils.h"

namespace
{
    // Distinctly named (mirrors CreateEvalReadbackSequence / CreateAddActorBindingSequence
    // in sibling sequencer test .cpp files) so anonymous-namespace symbols don't ODR-collide
    // when Unity merges these TUs: create a real /Game LevelSequence via the registered
    // sequencer.create handler so add_camera_track's LoadObject<ULevelSequence> can resolve
    // it. Empty path + nullptr on failure.
    ULevelSequence* CreateSectionRangeUnitsSequence(FAutomationTestBase& Test, FString& OutFullPath)
    {
        const FString SeqName = FString::Printf(TEXT("MCP_SectionRangeUnitsSeq_%s"),
            *FGuid::NewGuid().ToString(EGuidFormats::Digits));
        const FString DestFolder = TEXT("/Game/MCP_SectionRangeUnitsProbe");
        OutFullPath = FString::Printf(TEXT("%s/%s"), *DestFolder, *SeqName);

        TSharedPtr<FJsonObject> CreatePayload = MakeShared<FJsonObject>();
        CreatePayload->SetStringField(TEXT("name"), SeqName);
        CreatePayload->SetStringField(TEXT("path"), DestFolder);
        FTestResponseCapture CreateCapture;
        InvokeHandlerWithCapture(TEXT("sequencer.create"), CreatePayload, CreateCapture);

        if (!CreateCapture.bWasCalled || !CreateCapture.bSuccess ||
            !UEditorAssetLibrary::DoesAssetExist(OutFullPath))
        {
            Test.AddError(TEXT("Could not create a probe LevelSequence via sequencer.create — "
                               "the add_camera_track section-range repro cannot be exercised without it."));
            OutFullPath.Reset();
            return nullptr;
        }
        return Cast<ULevelSequence>(UEditorAssetLibrary::LoadAsset(OutFullPath));
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSequencerSectionRangeSecondsToTicksTest,
    "PinWright.Sequencer.SectionRange.CameraCutSpansRequestedSeconds",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSequencerSectionRangeSecondsToTicksTest::RunTest(const FString& Parameters)
{
    FScopedEditorWorldActorGuard WorldGuard;
    if (!GEditor)
    {
        AddError(TEXT("GEditor unavailable — cannot exercise sequencer.add_camera_track."));
        return false;
    }

    UWorld* World = GEditor->GetEditorWorldContext().World();
    if (!TestNotNull(TEXT("editor world present"), World))
    {
        return false;
    }

    // A real /Game LevelSequence for add_camera_track to author a camera-cut section into.
    FString FullPath;
    ULevelSequence* Sequence = CreateSectionRangeUnitsSequence(*this, FullPath);
    if (!Sequence)
    {
        return false;
    }
    ON_SCOPE_EXIT { CleanupTestAsset(FullPath); };

    UMovieScene* MovieScene = Sequence->GetMovieScene();
    if (!TestNotNull(TEXT("MovieScene present on the probe sequence"), MovieScene))
    {
        return false;
    }

    const FFrameRate TickResolution = MovieScene->GetTickResolution();
    const FFrameRate DisplayRate    = MovieScene->GetDisplayRate();

    const double EndTimeSeconds = 5.0;
    // The correct stored range end for a 5 s section: seconds in TICK-resolution frames.
    const FFrameNumber ExpectedEndTicks = TickResolution.AsFrameNumber(EndTimeSeconds);
    // What the defect produces: seconds in DISPLAY-rate frames fed to SetRange.
    const FFrameNumber DisplayFrameEnd  = DisplayRate.AsFrameTime(EndTimeSeconds).GetFrame();

    // Precondition: the two units must differ, or this fixture cannot demonstrate the bug.
    // At the default 24000 TickResolution / 24 fps DisplayRate they differ 1000x.
    if (!TestTrue(TEXT("fixture exposes the defect: TickResolution frames != DisplayRate frames "
                       "for the requested seconds (else units coincide and the bug is invisible)"),
            ExpectedEndTicks != DisplayFrameEnd))
    {
        return false;
    }

    // A live camera actor in the editor world — spawned exactly as sequencer.add_camera does,
    // so its GetPathName() is the loadable cameraActorPath add_camera_track expects.
    const FString CameraLabel = FString::Printf(TEXT("MCP_SectionRangeCam_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    ACameraActor* CameraActor = SpawnActorInActiveWorld<ACameraActor>(
        ACameraActor::StaticClass(), FVector::ZeroVector, FRotator::ZeroRotator, CameraLabel);
    if (!TestNotNull(TEXT("camera actor spawned in the editor world"), CameraActor))
    {
        return false;
    }

    // Bind the camera into the sequence before authoring the cut. add_camera_track resolves
    // the section binding by object identity (ULevelSequence::FindBindingFromObject) and
    // rejects an unbound camera with CAMERA_NOT_BOUND, so the camera this test names must
    // already be a bound possessable — exactly what sequencer.add_camera / add_actors produce.
    // Bind directly against the same editor world the handler resolves against (this test
    // drives the handler through the null-subsystem InvokeHandlerWithCapture, so the
    // subsystem-backed add_actors handler is not usable here).
    const FGuid CameraBindingGuid =
        MovieScene->AddPossessable(CameraActor->GetActorLabel(), CameraActor->GetClass());
    if (!TestTrue(TEXT("camera possessable created in the probe sequence"),
            MovieScene->FindPossessable(CameraBindingGuid) != nullptr))
    {
        return false;
    }
    Sequence->BindPossessableObject(CameraBindingGuid, *CameraActor, World);

    // Drive the production sequencer.add_camera_track handler for a 0..5 s cut.
    TSharedPtr<FJsonObject> AddPayload = MakeShared<FJsonObject>();
    AddPayload->SetStringField(TEXT("sequencePath"), FullPath);
    AddPayload->SetStringField(TEXT("cameraActorPath"), CameraActor->GetPathName());
    AddPayload->SetNumberField(TEXT("startTime"), 0.0);
    AddPayload->SetNumberField(TEXT("endTime"), EndTimeSeconds);

    FTestResponseCapture AddCapture;
    const bool bFound = InvokeHandlerWithCapture(TEXT("sequencer.add_camera_track"), AddPayload, AddCapture);
    if (!TestTrue(TEXT("sequencer.add_camera_track handler is registered and invoked"), bFound))
    {
        return false;
    }
    // Preconditions documenting the false success: the handler reports success:true both
    // pre- and post-fix. A failure here (e.g. CAMERA_LOAD_FAILED) is a fixture problem.
    if (!TestTrue(TEXT("sequencer.add_camera_track responded"), AddCapture.bWasCalled))
    {
        return false;
    }
    if (!TestTrue(TEXT("sequencer.add_camera_track reported success"), AddCapture.bSuccess))
    {
        AddError(FString::Printf(TEXT("add_camera_track failed (code=%s msg=%s) — fixture problem, "
                                      "not the section-range defect"), *AddCapture.ErrorCode, *AddCapture.Message));
        return false;
    }

    // Locate the camera-cut section the handler just authored.
    UMovieSceneTrack* CutBase = MovieScene->GetCameraCutTrack();
    UMovieSceneCameraCutTrack* CutTrack = CutBase ? Cast<UMovieSceneCameraCutTrack>(CutBase) : nullptr;
    if (!TestNotNull(TEXT("camera-cut track created by add_camera_track"), CutTrack))
    {
        return false;
    }
    const TArray<UMovieSceneSection*>& Sections = CutTrack->GetAllSections();
    if (!TestTrue(TEXT("camera-cut track has exactly one authored section"), Sections.Num() == 1))
    {
        return false;
    }
    UMovieSceneSection* Section = Sections[0];
    if (!TestNotNull(TEXT("camera-cut section present"), Section))
    {
        return false;
    }
    if (!TestTrue(TEXT("camera-cut section has a bounded start and end"),
            Section->HasStartFrame() && Section->HasEndFrame()))
    {
        return false;
    }

    const FFrameNumber ActualStart = Section->GetInclusiveStartFrame();
    const FFrameNumber ActualEnd   = Section->GetExclusiveEndFrame();
    const double ActualDurationSeconds =
        TickResolution.AsSeconds(FFrameTime(ActualEnd - ActualStart));

    AddInfo(FString::Printf(
        TEXT("camera-cut section stored range [%d, %d) ticks @ TickResolution %d/%d -> %.6f s "
             "(requested %.1f s; DisplayRate-frames-as-ticks bug would store end=%d)"),
        ActualStart.Value, ActualEnd.Value,
        TickResolution.Numerator, TickResolution.Denominator,
        ActualDurationSeconds, EndTimeSeconds, DisplayFrameEnd.Value));

    // CORRECT-BEHAVIOR ASSERTIONS (the defect surfaces here).
    // 1) The section's stored end frame must be the seconds expressed in TICK resolution
    //    (120000 at defaults), NOT the DisplayRate-frame value (120). Pre-fix ActualEnd == 120.
    TestEqual(TEXT("camera-cut section end frame is the requested seconds in TICK resolution "
                   "(not DisplayRate frames fed to SetRange)"),
        ActualEnd.Value, ExpectedEndTicks.Value);

    // 2) Converting the stored tick range back to seconds must recover the requested duration.
    //    Pre-fix the section spans ~0.005 s (1000x too short) and this fails.
    TestTrue(FString::Printf(
                 TEXT("camera-cut section spans the requested %.1f s when its tick range is "
                      "converted back to seconds (got %.6f s)"), EndTimeSeconds, ActualDurationSeconds),
        FMath::IsNearlyEqual(ActualDurationSeconds, EndTimeSeconds, 0.05));

    // Binding-identity check for the add_camera_track fix: the cut section must bind the
    // GUID of the camera named by cameraActorPath (resolved via FindBindingFromObject), not
    // the first camera-class possessable it happens to find.
    if (UMovieSceneCameraCutSection* CutSection = Cast<UMovieSceneCameraCutSection>(Section))
    {
        const FGuid BoundGuid = CutSection->GetCameraBindingID().GetGuid();
        TestTrue(*FString::Printf(
                     TEXT("camera-cut section binds the exact camera named by cameraActorPath "
                          "(bound=%s expected=%s)"),
                     *BoundGuid.ToString(), *CameraBindingGuid.ToString()),
            BoundGuid == CameraBindingGuid);
    }

    return true;
}
