// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for F-sequencer-track-state-readback:
// sequencer.set_track_muted / set_track_solo / set_track_locked write persisted
// per-track state (Track->SetEvalDisabled / Section->SetIsLocked), and the sequencer
// read surface must surface that state so the round-trip is verifiable. Drives the
// real production handlers (no copies): create a transient sequence with a track that
// has a section, mutate state through the set_track_* handlers, then read it back
// through sequencer.list_tracks and assert the new isEvalDisabled / allSectionsLocked
// fields reflect the writes. Reverting the readback fields makes these assertions fail.
#include "Misc/AutomationTest.h"
#include "Handlers/HandlerContext.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Tests/TestUtils.h"
#include "Tests/Sequencer/SequencerTestFixtures.h"

#include "LevelSequence.h"
#include "MovieScene.h"
#include "Tracks/MovieSceneLevelVisibilityTrack.h"
#include "UObject/Package.h"

// ============================================================================
// list_tracks surfaces eval-disable (mute/solo) and lock state set by the setters.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSequencerTrackStateReadbackTest,
    "PinWright.Sequencer.TrackStateReadback.ListTracksReflectsSetters",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSequencerTrackStateReadbackTest::RunTest(const FString& Parameters)
{
    FString ObjectPath;
    // The fixture is RF_Standalone, so the periodic suite GC keeps it alive; detach it so
    // it cannot answer a later /Engine/Transient asset-registry rescan.
    ON_SCOPE_EXIT
    {
        CleanupTestAsset(ObjectPath);
    };

    ULevelSequence* Sequence = SequencerTestFixtures::MakeTransientSequence(TEXT("TrackStateReadbackTest"), ObjectPath);
    if (!TestNotNull(TEXT("transient LevelSequence created"), Sequence))
    {
        return true;
    }

    // Create a master track that owns a section, via the production handler.
    {
        TSharedPtr<FJsonObject> AddPayload = MakeShared<FJsonObject>();
        AddPayload->SetStringField(TEXT("sequencePath"), ObjectPath);
        TArray<TSharedPtr<FJsonValue>> Names;
        Names.Add(MakeShared<FJsonValueString>(TEXT("Sublevel_Readback")));
        AddPayload->SetArrayField(TEXT("levelNames"), Names);
        AddPayload->SetBoolField(TEXT("bVisible"), true);
        FTestResponseCapture AddCapture;
        if (!TestTrue(TEXT("add_level_visibility_track handler found"),
                InvokeHandlerWithCapture(TEXT("sequencer.add_level_visibility_track"), AddPayload, AddCapture)))
        {
            return true;
        }
        if (!TestTrue(TEXT("add_level_visibility_track succeeded"), AddCapture.bSuccess))
        {
            return true;
        }
    }

    UMovieScene* MovieScene = Sequence->GetMovieScene();
    if (!TestNotNull(TEXT("MovieScene present"), MovieScene))
    {
        return true;
    }
    UMovieSceneLevelVisibilityTrack* Track = MovieScene->FindTrack<UMovieSceneLevelVisibilityTrack>();
    if (!TestNotNull(TEXT("visibility track created"), Track))
    {
        return true;
    }
    if (!TestTrue(TEXT("track has at least one section"), Track->GetAllSections().Num() > 0))
    {
        return true;
    }
    const FString TrackName = Track->GetName();

    // --- Baseline: list_tracks must carry the readback fields, both false. ---
    auto ListTracks = [&ObjectPath, this](FTestResponseCapture& OutCapture)
    {
        TSharedPtr<FJsonObject> ListPayload = MakeShared<FJsonObject>();
        ListPayload->SetStringField(TEXT("path"), ObjectPath);
        return InvokeHandlerWithCapture(TEXT("sequencer.list_tracks"), ListPayload, OutCapture);
    };

    {
        FTestResponseCapture ListCapture;
        if (!TestTrue(TEXT("list_tracks handler found (baseline)"), ListTracks(ListCapture)))
        {
            return true;
        }
        TestTrue(TEXT("list_tracks succeeded (baseline)"), ListCapture.bSuccess);
        TSharedPtr<FJsonObject> Entry = SequencerTestFixtures::FindTrackEntryByName(ListCapture.Result, TrackName);
        if (!TestNotNull(TEXT("track entry present in baseline list"), Entry.Get()))
        {
            return true;
        }
        bool bEvalDisabled = true;   // wrong default so a missing field fails the test
        bool bAllLocked = true;
        TestTrue(TEXT("baseline entry has isEvalDisabled field"),
            Entry->TryGetBoolField(TEXT("isEvalDisabled"), bEvalDisabled));
        TestFalse(TEXT("baseline isEvalDisabled is false"), bEvalDisabled);
        TestTrue(TEXT("baseline entry has allSectionsLocked field"),
            Entry->TryGetBoolField(TEXT("allSectionsLocked"), bAllLocked));
        TestFalse(TEXT("baseline allSectionsLocked is false"), bAllLocked);
    }

    // --- Mutate state through the production setters. ---
    {
        TSharedPtr<FJsonObject> MutePayload = MakeShared<FJsonObject>();
        MutePayload->SetStringField(TEXT("path"), ObjectPath);
        MutePayload->SetStringField(TEXT("trackName"), TrackName);
        MutePayload->SetBoolField(TEXT("muted"), true);
        FTestResponseCapture MuteCapture;
        TestTrue(TEXT("set_track_muted handler found"),
            InvokeHandlerWithCapture(TEXT("sequencer.set_track_muted"), MutePayload, MuteCapture));
        TestTrue(TEXT("set_track_muted succeeded"), MuteCapture.bSuccess);

        TSharedPtr<FJsonObject> LockPayload = MakeShared<FJsonObject>();
        LockPayload->SetStringField(TEXT("path"), ObjectPath);
        LockPayload->SetStringField(TEXT("trackName"), TrackName);
        LockPayload->SetBoolField(TEXT("locked"), true);
        FTestResponseCapture LockCapture;
        TestTrue(TEXT("set_track_locked handler found"),
            InvokeHandlerWithCapture(TEXT("sequencer.set_track_locked"), LockPayload, LockCapture));
        TestTrue(TEXT("set_track_locked succeeded"), LockCapture.bSuccess);
    }

    // Sanity: the engine state itself is what we asserted (proves we read the real flag).
    TestTrue(TEXT("engine track is eval-disabled after set_track_muted"), Track->IsEvalDisabled());

    // --- Read back: list_tracks must now report the mutated state. ---
    {
        FTestResponseCapture ListCapture;
        if (!TestTrue(TEXT("list_tracks handler found (post-write)"), ListTracks(ListCapture)))
        {
            return true;
        }
        TestTrue(TEXT("list_tracks succeeded (post-write)"), ListCapture.bSuccess);
        TSharedPtr<FJsonObject> Entry = SequencerTestFixtures::FindTrackEntryByName(ListCapture.Result, TrackName);
        if (!TestNotNull(TEXT("track entry present in post-write list"), Entry.Get()))
        {
            return true;
        }
        bool bEvalDisabled = false;
        bool bAllLocked = false;
        TestTrue(TEXT("post-write entry has isEvalDisabled field"),
            Entry->TryGetBoolField(TEXT("isEvalDisabled"), bEvalDisabled));
        TestTrue(TEXT("post-write isEvalDisabled reflects set_track_muted"), bEvalDisabled);
        TestTrue(TEXT("post-write entry has allSectionsLocked field"),
            Entry->TryGetBoolField(TEXT("allSectionsLocked"), bAllLocked));
        TestTrue(TEXT("post-write allSectionsLocked reflects set_track_locked"), bAllLocked);
    }

    return true;
}
