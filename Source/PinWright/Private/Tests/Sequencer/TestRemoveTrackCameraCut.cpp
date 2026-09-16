// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for B-sequencer-remove-track-misses-camera-cut-slot:
// sequencer.remove_track must be able to remove the camera-cut track, which lives on
// the dedicated MovieScene->GetCameraCutTrack() slot and NOT in GetTracks() or any
// binding. Before the fix, remove_track scanned only GetTracks() and each
// Binding.GetTracks(), so the camera-cut trackName that list_tracks (and
// get_camera_cut_track) hands back fell through to TRACK_NOT_FOUND — a track the
// plugin's own readers surface could be created (add_camera_track) and enumerated
// (list_tracks) but never removed. The fix adds a branch that matches the slot by
// GetName()/GetDisplayName() and clears it via the engine's dedicated
// RemoveCameraCutTrack() (a bare MovieScene->RemoveTrack() would not clear the slot).
//
// Drives the real production sequencer.remove_track handler (no copy): builds a
// transient sequence, authors a camera-cut track via the engine API
// (MovieScene->AddCameraCutTrack — the same call the add_camera_track handler uses),
// then removes it through the handler by the exact trackName list_tracks reports
// (Track->GetName()) and asserts the dedicated slot is cleared. Reverting the
// handler's camera-cut branch makes the removal return TRACK_NOT_FOUND and this fails.
#include "Misc/AutomationTest.h"
#include "Misc/EngineVersionComparison.h"
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
// remove_track removes the camera-cut track by the name list_tracks reports,
// and clears the dedicated GetCameraCutTrack() slot.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSequencerRemoveTrackCameraCutTest,
    "PinWright.Sequencer.RemoveTrack.RemovesCameraCutTrack",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSequencerRemoveTrackCameraCutTest::RunTest(const FString& Parameters)
{
    // remove_track resolves the sequence through SequenceHelpers::ResolveSequencePath,
    // which for a supplied path does `if (UEditorAssetLibrary::DoesAssetExist(Path))
    // UEditorAssetLibrary::LoadAsset(Path)`. On this engine DoesAssetExist returns true
    // for a loaded in-memory /Engine/Transient sequence, so LoadAsset is attempted and
    // logs "LoadAsset failed: '...' is not a valid asset" before the helper falls back to
    // the raw path (which LoadObject then resolves fine). That benign log-error is an
    // artifact of driving the handler with a transient fixture — it never fires for real
    // /Game sequences in production — and every sibling sequencer test that drives a
    // handler on a transient sequence hits it. Tolerate it (negative Occurrences =
    // ignore 0-or-more) so it does not mask this test's real assertions.
#if UE_VERSION_OLDER_THAN(5, 6, 0)
    // 5.3, 5.4 and 5.5 do NOT fire it: DoesAssetExist reports false for the in-memory transient
    // sequence, so ResolveSequencePath skips LoadAsset entirely and goes straight to the
    // raw-path fallback. Measured over a full suite run on each - zero occurrences inside
    // this test's window. (Two successive claims that an older engine "fires it
    // deterministically" were extrapolations and both were wrong: first for 5.4, then for 5.3.
    // On 5.3 the message does occur elsewhere in the suite, but never between this test's
    // Test Started and Test Completed.) None of the three has a "0-or-more" spelling - pre-5.6
    // AutomationTest.cpp understands only >0 = exact and 0 = one-or-more - and registering the
    // expectation anyway fails the test on a message that is not there, so nothing is
    // registered here. If the error ever does fire it surfaces as an unexpected error, which
    // is the correct signal rather than a silenced one.
#else
    AddExpectedErrorPlain(TEXT("is not a valid asset"),
        EAutomationExpectedErrorFlags::Contains, /*Occurrences (<0 = ignore any)*/ -1);
#endif

    // Authors a camera-cut track on a fresh transient sequence via the engine API
    // (the same AddCameraCutTrack call the add_camera_track handler makes) and returns
    // the sequence + its object path. The cut track is verified present on the dedicated
    // slot and absent from GetTracks() before the caller exercises removal.
    auto MakeSequenceWithCameraCut = [this](const TCHAR* Prefix, FString& OutObjectPath,
        UMovieScene*& OutMovieScene, FString& OutTrackName, FString& OutDisplayName) -> bool
    {
        ULevelSequence* Sequence = SequencerTestFixtures::MakeTransientSequence(Prefix, OutObjectPath);
        if (!TestNotNull(TEXT("transient LevelSequence created"), Sequence))
        {
            return false;
        }
        OutMovieScene = Sequence->GetMovieScene();
        if (!TestNotNull(TEXT("MovieScene present"), OutMovieScene))
        {
            return false;
        }
        UMovieSceneTrack* CutBase = OutMovieScene->AddCameraCutTrack(UMovieSceneCameraCutTrack::StaticClass());
        UMovieSceneCameraCutTrack* CameraCutTrack = Cast<UMovieSceneCameraCutTrack>(CutBase);
        if (!TestNotNull(TEXT("camera-cut track authored on MovieScene"), CameraCutTrack))
        {
            return false;
        }
        // Sanity: it is NOT in GetTracks() — confirms the master-track loop alone would miss it.
        TestFalse(TEXT("camera-cut track is absent from GetTracks()"),
            OutMovieScene->GetTracks().Contains(CameraCutTrack));
        if (!TestNotNull(TEXT("GetCameraCutTrack() returns the authored track"),
                OutMovieScene->GetCameraCutTrack()))
        {
            return false;
        }
        // The name list_tracks would hand back (EmitTrack sets trackName = Track->GetName()).
        OutTrackName = CameraCutTrack->GetName();
        OutDisplayName = CameraCutTrack->GetDisplayName().ToString();
        return true;
    };

    auto RemoveTrack = [this](const FString& ObjectPath, const FString& TrackName,
        FTestResponseCapture& OutCapture) -> bool
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("path"), ObjectPath);
        Payload->SetStringField(TEXT("trackName"), TrackName);
        return InvokeHandlerWithCapture(TEXT("sequencer.remove_track"), Payload, OutCapture);
    };

    // --- Case 1: remove by the exact trackName list_tracks reports (Track->GetName()). ---
    // This is the load-bearing round-trip: a track the readers surface must be removable
    // by the name they report. Reverting the fix returns TRACK_NOT_FOUND here.
    {
        FString ObjectPath, TrackName, DisplayName;
        UMovieScene* MovieScene = nullptr;
        // The fixture is RF_Standalone, so the periodic suite GC keeps it alive; detach it
        // so it cannot answer a later /Engine/Transient asset-registry rescan.
        ON_SCOPE_EXIT
        {
            CleanupTestAsset(ObjectPath);
        };

        if (!MakeSequenceWithCameraCut(TEXT("RemoveTrackCameraCutByName"), ObjectPath,
                MovieScene, TrackName, DisplayName))
        {
            return true;
        }

        // Guard: a bogus name must still yield TRACK_NOT_FOUND and leave the slot intact,
        // proving the branch matches on the name rather than blindly clearing the slot.
        {
            FTestResponseCapture BogusCapture;
            if (!TestTrue(TEXT("remove_track handler found (bogus name)"),
                    RemoveTrack(ObjectPath, TEXT("NoSuchTrack_ZZZ"), BogusCapture)))
            {
                return true;
            }
            TestFalse(TEXT("remove_track rejects a non-matching name"), BogusCapture.bSuccess);
            TestEqual(TEXT("remove_track returns TRACK_NOT_FOUND for a non-matching name"),
                BogusCapture.ErrorCode, FString(TEXT("TRACK_NOT_FOUND")));
            TestNotNull(TEXT("camera-cut slot still present after a non-matching remove"),
                MovieScene->GetCameraCutTrack());
        }

        // Remove by the reported trackName -> success, and the dedicated slot is cleared.
        {
            FTestResponseCapture Capture;
            if (!TestTrue(TEXT("remove_track handler found (by name)"),
                    RemoveTrack(ObjectPath, TrackName, Capture)))
            {
                return true;
            }
            TestTrue(TEXT("remove_track succeeds for the camera-cut trackName list_tracks reports"),
                Capture.bSuccess);
            FString EchoedName;
            if (Capture.Result.IsValid())
            {
                Capture.Result->TryGetStringField(TEXT("trackName"), EchoedName);
            }
            TestEqual(TEXT("remove_track echoes the removed camera-cut trackName"),
                EchoedName, TrackName);
            TestNull(TEXT("GetCameraCutTrack() slot is cleared after remove_track"),
                MovieScene->GetCameraCutTrack());
        }
    }

    // --- Case 2: remove by the displayName list_tracks also reports ("Camera Cuts"). ---
    // The repro attempt tried the displayName too; the branch accepts it as well.
    {
        FString ObjectPath, TrackName, DisplayName;
        UMovieScene* MovieScene = nullptr;
        // The fixture is RF_Standalone, so the periodic suite GC keeps it alive; detach it
        // so it cannot answer a later /Engine/Transient asset-registry rescan.
        ON_SCOPE_EXIT
        {
            CleanupTestAsset(ObjectPath);
        };

        if (!MakeSequenceWithCameraCut(TEXT("RemoveTrackCameraCutByDisplayName"), ObjectPath,
                MovieScene, TrackName, DisplayName))
        {
            return true;
        }

        FTestResponseCapture Capture;
        if (!TestTrue(TEXT("remove_track handler found (by displayName)"),
                RemoveTrack(ObjectPath, DisplayName, Capture)))
        {
            return true;
        }
        TestTrue(TEXT("remove_track succeeds for the camera-cut displayName"), Capture.bSuccess);
        TestNull(TEXT("GetCameraCutTrack() slot is cleared after remove_track by displayName"),
            MovieScene->GetCameraCutTrack());
    }

    return true;
}
