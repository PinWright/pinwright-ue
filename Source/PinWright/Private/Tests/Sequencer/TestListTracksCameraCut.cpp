// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for E-sequencer-list-tracks-omits-camera-cut-undocumented:
// sequencer.list_tracks must enumerate the camera-cut track, which lives on the
// dedicated MovieScene->GetCameraCutTrack() slot and not in GetTracks() — so the
// master-track loop would otherwise miss it. The camera-cut entry is tagged with
// isCameraCutTrack=true so a generic "list the tracks" caller gets a complete picture
// instead of silently dropping an authored cut track.
//
// Drives the real production sequencer.list_tracks handler (no copy): builds a
// transient sequence, authors a camera-cut track via the engine API
// (MovieScene->AddCameraCutTrack — the same call the add_camera_track handler uses),
// then reads it back through the handler and asserts a tracks[] entry carries
// isCameraCutTrack=true. Reverting the handler's camera-cut emission makes this fail.
#include "Misc/AutomationTest.h"
#include "Handlers/HandlerContext.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Tests/TestUtils.h"
#include "Tests/Sequencer/SequencerTestFixtures.h"

#include "LevelSequence.h"
#include "MovieScene.h"
#include "Tracks/MovieSceneCameraCutTrack.h"
#include "UObject/Package.h"

// ============================================================================
// list_tracks enumerates the camera-cut track with an isCameraCutTrack discriminator.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSequencerListTracksCameraCutTest,
    "PinWright.Sequencer.ListTracks.IncludesCameraCutTrack",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSequencerListTracksCameraCutTest::RunTest(const FString& Parameters)
{
    FString ObjectPath;
    // The fixture is RF_Standalone, so the periodic suite GC keeps it alive; detach it so
    // it cannot answer a later /Engine/Transient asset-registry rescan.
    ON_SCOPE_EXIT
    {
        CleanupTestAsset(ObjectPath);
    };

    ULevelSequence* Sequence = SequencerTestFixtures::MakeTransientSequence(TEXT("ListTracksCameraCutTest"), ObjectPath);
    if (!TestNotNull(TEXT("transient LevelSequence created"), Sequence))
    {
        return true;
    }
    UMovieScene* MovieScene = Sequence->GetMovieScene();
    if (!TestNotNull(TEXT("MovieScene present"), MovieScene))
    {
        return true;
    }

    auto ListTracks = [&ObjectPath, this](FTestResponseCapture& OutCapture)
    {
        TSharedPtr<FJsonObject> ListPayload = MakeShared<FJsonObject>();
        ListPayload->SetStringField(TEXT("path"), ObjectPath);
        return InvokeHandlerWithCapture(TEXT("sequencer.list_tracks"), ListPayload, OutCapture);
    };

    // Counts tracks[] entries whose isCameraCutTrack field is true.
    auto CameraCutEntryCount = [](const TSharedPtr<FJsonObject>& Result) -> int32
    {
        int32 Count = 0;
        SequencerTestFixtures::ForEachTrackEntry(Result, [&](const TSharedPtr<FJsonObject>& Entry)
        {
            bool bIsCameraCut = false;
            if (Entry->TryGetBoolField(TEXT("isCameraCutTrack"), bIsCameraCut) && bIsCameraCut)
            {
                ++Count;
            }
        });
        return Count;
    };

    // --- Baseline: no camera-cut track authored, so no camera-cut entry. ---
    {
        FTestResponseCapture ListCapture;
        if (!TestTrue(TEXT("list_tracks handler found (baseline)"), ListTracks(ListCapture)))
        {
            return true;
        }
        TestTrue(TEXT("list_tracks succeeded (baseline)"), ListCapture.bSuccess);
        TestEqual(TEXT("no camera-cut entry before authoring one"),
            CameraCutEntryCount(ListCapture.Result), 0);
    }

    // --- Author a camera-cut track on the dedicated slot (engine API, same call the
    //     add_camera_track handler makes). This is the slot GetTracks() never returns. ---
    UMovieSceneTrack* CutBase = MovieScene->AddCameraCutTrack(UMovieSceneCameraCutTrack::StaticClass());
    UMovieSceneCameraCutTrack* CameraCutTrack = Cast<UMovieSceneCameraCutTrack>(CutBase);
    if (!TestNotNull(TEXT("camera-cut track authored on MovieScene"), CameraCutTrack))
    {
        return true;
    }
    // Sanity: it is NOT in GetTracks() — confirms the master-track loop alone would miss it.
    TestFalse(TEXT("camera-cut track is absent from GetTracks()"),
        MovieScene->GetTracks().Contains(CameraCutTrack));
    if (!TestNotNull(TEXT("GetCameraCutTrack() returns the authored track"),
            MovieScene->GetCameraCutTrack()))
    {
        return true;
    }

    // --- Read back: list_tracks must now surface exactly one camera-cut entry, tagged. ---
    {
        FTestResponseCapture ListCapture;
        if (!TestTrue(TEXT("list_tracks handler found (post-author)"), ListTracks(ListCapture)))
        {
            return true;
        }
        TestTrue(TEXT("list_tracks succeeded (post-author)"), ListCapture.bSuccess);
        TestEqual(TEXT("list_tracks enumerates the camera-cut track with isCameraCutTrack=true"),
            CameraCutEntryCount(ListCapture.Result), 1);
    }

    return true;
}
