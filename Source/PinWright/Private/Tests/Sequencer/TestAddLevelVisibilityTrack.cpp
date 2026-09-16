// Copyright (c) 2026 Alexander Penkin. MIT License.

// Unit tests for sequencer.add_level_visibility_track
#include "Misc/AutomationTest.h"
#include "Handlers/HandlerContext.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Tests/TestUtils.h"
#include "Tests/Sequencer/SequencerTestFixtures.h"

#include "LevelSequence.h"
#include "MovieScene.h"
#include "Tracks/MovieSceneLevelVisibilityTrack.h"
#include "Sections/MovieSceneLevelVisibilitySection.h"
#include "UObject/Package.h"


// ============================================================================
// sequencer.add_level_visibility_track  (REQ: sequencePath, levelNames, bVisible)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAddLevelVisibilityTrackPopulatesSectionTest,
    "PinWright.Sequencer.AddLevelVisibilityTrack.PopulatesSection",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAddLevelVisibilityTrackPopulatesSectionTest::RunTest(const FString& Parameters)
{
    FString ObjectPath;
    // The fixture is RF_Standalone, so the periodic suite GC keeps it alive; detach it so
    // it cannot answer a later /Engine/Transient asset-registry rescan.
    ON_SCOPE_EXIT
    {
        CleanupTestAsset(ObjectPath);
    };

    ULevelSequence* Sequence = SequencerTestFixtures::MakeTransientSequence(TEXT("LevelVisTrackTest"), ObjectPath);
    if (!TestNotNull(TEXT("transient LevelSequence created"), Sequence))
    {
        return true;
    }

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("sequencePath"), ObjectPath);
    TArray<TSharedPtr<FJsonValue>> Names;
    Names.Add(MakeShared<FJsonValueString>(TEXT("Sublevel_A")));
    Names.Add(MakeShared<FJsonValueString>(TEXT("Sublevel_B")));
    Payload->SetArrayField(TEXT("levelNames"), Names);
    Payload->SetBoolField(TEXT("bVisible"), false);
    Payload->SetNumberField(TEXT("rowIndex"), 2);

    FTestResponseCapture Capture;
    TestTrue(TEXT("sequencer.add_level_visibility_track handler found"),
        InvokeHandlerWithCapture(TEXT("sequencer.add_level_visibility_track"), Payload, Capture));
    TestTrue(TEXT("handler reported success"), Capture.bSuccess);

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
    const TArray<UMovieSceneSection*>& Sections = Track->GetAllSections();
    if (!TestEqual(TEXT("one section on track"), Sections.Num(), 1))
    {
        return true;
    }
    UMovieSceneLevelVisibilitySection* Section = Cast<UMovieSceneLevelVisibilitySection>(Sections[0]);
    if (!TestNotNull(TEXT("section is LevelVisibility"), Section))
    {
        return true;
    }
    TestEqual(TEXT("LevelNames count"), Section->GetLevelNames().Num(), 2);
    TestEqual(TEXT("visibility hidden"), static_cast<int32>(Section->GetVisibility()), static_cast<int32>(ELevelVisibility::Hidden));
    TestEqual(TEXT("rowIndex applied"), Section->GetRowIndex(), 2);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAddLevelVisibilityTrackReuseTrackTest,
    "PinWright.Sequencer.AddLevelVisibilityTrack.ReuseTrack",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAddLevelVisibilityTrackReuseTrackTest::RunTest(const FString& Parameters)
{
    FString ObjectPath;
    // The fixture is RF_Standalone, so the periodic suite GC keeps it alive; detach it so
    // it cannot answer a later /Engine/Transient asset-registry rescan.
    ON_SCOPE_EXIT
    {
        CleanupTestAsset(ObjectPath);
    };

    ULevelSequence* Sequence = SequencerTestFixtures::MakeTransientSequence(TEXT("LevelVisTrackTest"), ObjectPath);
    if (!TestNotNull(TEXT("transient LevelSequence created"), Sequence))
    {
        return true;
    }

    auto BuildPayload = [&ObjectPath](bool bVisible)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("sequencePath"), ObjectPath);
        TArray<TSharedPtr<FJsonValue>> Names;
        Names.Add(MakeShared<FJsonValueString>(TEXT("Sublevel_Reuse")));
        Payload->SetArrayField(TEXT("levelNames"), Names);
        Payload->SetBoolField(TEXT("bVisible"), bVisible);
        Payload->SetBoolField(TEXT("reuseTrack"), true);
        return Payload;
    };

    FTestResponseCapture FirstCapture;
    TestTrue(TEXT("first add succeeded"),
        InvokeHandlerWithCapture(TEXT("sequencer.add_level_visibility_track"), BuildPayload(true), FirstCapture));
    TestTrue(TEXT("first response bSuccess"), FirstCapture.bSuccess);

    FTestResponseCapture SecondCapture;
    TestTrue(TEXT("second add succeeded"),
        InvokeHandlerWithCapture(TEXT("sequencer.add_level_visibility_track"), BuildPayload(false), SecondCapture));
    TestTrue(TEXT("second response bSuccess"), SecondCapture.bSuccess);

    bool bReused = false;
    if (SecondCapture.Result.IsValid())
    {
        SecondCapture.Result->TryGetBoolField(TEXT("reused"), bReused);
    }
    TestTrue(TEXT("second add reused existing track"), bReused);

    UMovieScene* MovieScene = Sequence->GetMovieScene();
    if (!TestNotNull(TEXT("MovieScene present"), MovieScene))
    {
        return true;
    }
    int32 TrackCount = 0;
    for (UMovieSceneTrack* T : MovieScene->GetTracks())
    {
        if (Cast<UMovieSceneLevelVisibilityTrack>(T)) { ++TrackCount; }
    }
    TestEqual(TEXT("exactly one visibility track"), TrackCount, 1);

    UMovieSceneLevelVisibilityTrack* Track = MovieScene->FindTrack<UMovieSceneLevelVisibilityTrack>();
    if (!TestNotNull(TEXT("visibility track present"), Track))
    {
        return true;
    }
    TestEqual(TEXT("two sections on reused track"), Track->GetAllSections().Num(), 2);
    return true;
}
