// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression tests for B-sequencer-empty-trackname-matches-first:
// every trackName lookup (add_section, set_track_muted, set_track_solo, set_track_locked,
// remove_track) goes through SequenceHelpers::TrackMatchesIdentifier, which substring-matched
// with FString::Contains. Contains("") is true, so a call that omitted trackName resolved to
// whichever track was walked first and mutated it; remove_track deleted it, and without a
// transaction the delete could not be undone. The fix makes an empty identifier match nothing
// (TRACK_NOT_FOUND, nothing touched) and wraps remove_track in an FScopedTransaction.
//
// Drives the real production handlers on a transient sequence with two master tracks.
#include "Misc/AutomationTest.h"
#include "Misc/EngineVersionComparison.h"
#include "Handlers/HandlerContext.h"
#include "Dom/JsonObject.h"
#include "Tests/TestUtils.h"
#include "Tests/Sequencer/SequencerTestFixtures.h"

#include "Editor.h"
#include "Editor/Transactor.h"
#include "LevelSequence.h"
#include "MovieScene.h"
#include "Tracks/MovieSceneLevelVisibilityTrack.h"

namespace TrackLookupEmptyNameTestUtils
{
    // See TestRemoveTrackCameraCut.cpp: driving a handler on a transient sequence logs a benign
    // "is not a valid asset" on 5.6+ only (pre-5.6 has no 0-or-more spelling).
    inline void ExpectTransientPathNoise(FAutomationTestBase& Test)
    {
#if !UE_VERSION_OLDER_THAN(5, 6, 0)
        Test.AddExpectedErrorPlain(TEXT("is not a valid asset"),
            EAutomationExpectedErrorFlags::Contains, -1);
#endif
    }

    // Transient sequence with two section-less master tracks, A then B.
    inline UMovieScene* MakeTwoTrackSequence(const TCHAR* Prefix, FString& OutObjectPath,
        UMovieSceneTrack*& OutA, UMovieSceneTrack*& OutB)
    {
        ULevelSequence* Sequence = SequencerTestFixtures::MakeTransientSequence(Prefix, OutObjectPath);
        UMovieScene* MovieScene = Sequence ? Sequence->GetMovieScene() : nullptr;
        if (!MovieScene)
        {
            return nullptr;
        }
        OutA = MovieScene->AddTrack(UMovieSceneLevelVisibilityTrack::StaticClass());
        OutB = MovieScene->AddTrack(UMovieSceneLevelVisibilityTrack::StaticClass());
        return (OutA && OutB) ? MovieScene : nullptr;
    }

    // Invoke Method on ObjectPath; trackName is set only when TrackName is non-null.
    inline FTestResponseCapture Invoke(const TCHAR* Method, const FString& ObjectPath,
        const TCHAR* TrackName)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("path"), ObjectPath);
        if (TrackName)
        {
            Payload->SetStringField(TEXT("trackName"), TrackName);
        }
        FTestResponseCapture Capture;
        InvokeHandlerWithCapture(Method, Payload, Capture);
        return Capture;
    }
}

// ============================================================================
// An omitted or empty trackName resolves to no track on every lookup verb.
// Before the fix each of these calls succeeded against track A.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSequencerTrackLookupEmptyNameTest,
    "PinWright.Sequencer.TrackLookup.EmptyNameMatchesNoTrack",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSequencerTrackLookupEmptyNameTest::RunTest(const FString& Parameters)
{
    using namespace TrackLookupEmptyNameTestUtils;
    ExpectTransientPathNoise(*this);

    FString ObjectPath;
    ON_SCOPE_EXIT
    {
        CleanupTestAsset(ObjectPath);
    };
    UMovieSceneTrack* TrackA = nullptr;
    UMovieSceneTrack* TrackB = nullptr;
    UMovieScene* MovieScene = MakeTwoTrackSequence(TEXT("TrackLookupEmptyName"), ObjectPath, TrackA, TrackB);
    if (!TestNotNull(TEXT("two-track transient sequence built"), MovieScene))
    {
        return true;
    }

    const TCHAR* Methods[] = {
        TEXT("sequencer.remove_track"),
        TEXT("sequencer.set_track_muted"),
        TEXT("sequencer.set_track_solo"),
        TEXT("sequencer.set_track_locked"),
        TEXT("sequencer.add_section"),
    };
    const TCHAR* Names[] = { nullptr, TEXT("") };
    for (const TCHAR* Method : Methods)
    {
        for (const TCHAR* Name : Names)
        {
            const FString Label = FString::Printf(TEXT("%s with trackName %s"), Method,
                Name ? TEXT("empty") : TEXT("omitted"));
            const FTestResponseCapture Capture = Invoke(Method, ObjectPath, Name);
            TestFalse(*FString::Printf(TEXT("%s is rejected"), *Label), Capture.bSuccess);
            TestEqual(*FString::Printf(TEXT("%s returns TRACK_NOT_FOUND"), *Label),
                Capture.ErrorCode, FString(TEXT("TRACK_NOT_FOUND")));
        }
    }

    TestEqual(TEXT("no track was removed"), MovieScene->GetTracks().Num(), 2);
    TestFalse(TEXT("track A was not muted/soloed"), TrackA->IsEvalDisabled());
    TestFalse(TEXT("track B was not muted/soloed"), TrackB->IsEvalDisabled());
    TestEqual(TEXT("no section was added to track A"), TrackA->GetAllSections().Num(), 0);
    TestEqual(TEXT("no section was added to track B"), TrackB->GetAllSections().Num(), 0);
    return true;
}

// ============================================================================
// remove_track by name still removes exactly that track, and the removal is one
// undoable transaction: GEditor->UndoTransaction() restores it. Before the fix the
// handler opened no transaction, so the top of the undo stack was not the removal and
// the track could not be brought back.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSequencerRemoveTrackUndoTest,
    "PinWright.Sequencer.RemoveTrack.NamedRemovalIsUndoable",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSequencerRemoveTrackUndoTest::RunTest(const FString& Parameters)
{
    using namespace TrackLookupEmptyNameTestUtils;
    ExpectTransientPathNoise(*this);

    FString ObjectPath;
    ON_SCOPE_EXIT
    {
        CleanupTestAsset(ObjectPath);
    };
    UMovieSceneTrack* TrackA = nullptr;
    UMovieSceneTrack* TrackB = nullptr;
    UMovieScene* MovieScene = MakeTwoTrackSequence(TEXT("RemoveTrackUndo"), ObjectPath, TrackA, TrackB);
    if (!TestNotNull(TEXT("two-track transient sequence built"), MovieScene)
        || !TestNotNull(TEXT("editor transaction buffer available"), GEditor ? GEditor->Trans.Get() : nullptr))
    {
        return true;
    }

    const FString NameB = TrackB->GetName();
    const FTestResponseCapture Capture = Invoke(TEXT("sequencer.remove_track"), ObjectPath, *NameB);
    if (!TestTrue(TEXT("remove_track by name succeeds"), Capture.bSuccess))
    {
        return true;
    }
    FString Echoed;
    if (Capture.Result.IsValid())
    {
        Capture.Result->TryGetStringField(TEXT("trackName"), Echoed);
    }
    TestEqual(TEXT("remove_track echoes the named track"), Echoed, NameB);
    TestEqual(TEXT("exactly one track remains"), MovieScene->GetTracks().Num(), 1);
    TestTrue(TEXT("the unnamed track A is the survivor"), MovieScene->GetTracks().Contains(TrackA));
    TestFalse(TEXT("the named track B is gone"), MovieScene->GetTracks().Contains(TrackB));

    // Undo only our own transaction: without it the next undo would revert an unrelated one.
    if (!TestEqual(TEXT("remove_track is the transaction on top of the undo stack"),
            GEditor->Trans->GetUndoContext().Title.ToString(), FString(TEXT("Remove Sequencer Track"))))
    {
        return true;
    }
    TestTrue(TEXT("undo succeeds"), GEditor->UndoTransaction(/*bCanRedo=*/false));
    TestEqual(TEXT("undo restores both tracks"), MovieScene->GetTracks().Num(), 2);
    TestTrue(TEXT("undo restores the removed track B"), MovieScene->GetTracks().Contains(TrackB));
    return true;
}
