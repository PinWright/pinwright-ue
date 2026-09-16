// Copyright (c) 2026 Alexander Penkin. MIT License.

// sequencer.add_track used to answer with `TrackName.IsEmpty() ? TrackType : TrackName` — the
// caller's own request read back out. Six verbs (add_section, set_track_muted, set_track_solo,
// set_track_locked, remove_track, list_sections) resolve a track by that string, so a caller who
// supplied trackName got back an identifier guaranteed to answer TRACK_NOT_FOUND on its very next
// call, and the requested name had never been written to the track at all.
//
// These tests drive the real production handlers and assert the two directions that matter:
//   - the identifier add_track returns is the one the setters actually resolve (positive), and
//     the requested trackName really landed on the track's DisplayName in the graph;
//   - add_track REPORTS FAILURE, and adds nothing, when the requested trackName cannot be
//     applied because the track class is outside the UMovieSceneNameableTrack subtree.
// Reverting the fix makes both fail: the first because the returned identifier stops resolving,
// the second because the verb goes back to succeeding with an unapplied name.
#include "Misc/AutomationTest.h"
#include "Handlers/HandlerContext.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Tests/TestUtils.h"
#include "Tests/Sequencer/SequencerTestFixtures.h"

#include "LevelSequence.h"
#include "MovieScene.h"
#include "MovieSceneSection.h"
#include "MovieSceneTrack.h"
#include "Misc/FrameNumber.h"
#include "Misc/FrameRate.h"

// Named (not anonymous) namespace: Unity merges the test translation units, and an anonymous
// namespace at file scope is the documented ODR hazard in this codebase.
namespace AddTrackIdentifierTestUtils
{
    // Referenced by path string only (never linked), so this file needs no track headers.
    // UMovieSceneAudioTrack derives from UMovieSceneNameableTrack, so it CAN carry a trackName
    // (MovieSceneAudioTrack.h:22-23).
    inline const TCHAR* NameableTrackTypePath()
    {
        return TEXT("/Script/MovieSceneTracks.MovieSceneAudioTrack");
    }
    // UMovieSceneSpawnTrack derives straight from UMovieSceneTrack (MovieSceneSpawnTrack.h:30-31),
    // so there is nowhere on it to store a display name.
    inline const TCHAR* UnnameableTrackTypePath()
    {
        return TEXT("/Script/MovieScene.MovieSceneSpawnTrack");
    }

    // The tick-resolution frame sequencer.add_section must store for a DISPLAY-rate frame
    // number, re-derived from the MovieScene's own GetDisplayRate()/GetTickResolution()
    // rather than from a copy of the handler's arithmetic. Mirrors
    // SequenceHelpers::DisplayFrameToTick, which is a file-internal static in
    // SequenceHandler.cpp and so cannot be called from a test TU.
    inline FFrameNumber ExpectedSectionTick(UMovieScene* MovieScene, double DisplayFrame)
    {
        return FFrameRate::TransformTime(FFrameTime(FFrameNumber(static_cast<int32>(DisplayFrame))),
            MovieScene->GetDisplayRate(), MovieScene->GetTickResolution())
            .FloorToFrame();
    }
}

// ============================================================================
// The trackName add_track returns resolves in the verbs that consume it, and the
// requested name is on the track rather than only in the response.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSequencerAddTrackIdentifierResolvesTest,
    "PinWright.Sequencer.AddTrackIdentifier.ReturnedNameResolvesInSetters",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSequencerAddTrackIdentifierResolvesTest::RunTest(const FString& Parameters)
{
    FString ObjectPath;
    // The fixture is RF_Standalone, so the periodic suite GC keeps it alive; detach it so
    // it cannot answer a later /Engine/Transient asset-registry rescan.
    ON_SCOPE_EXIT
    {
        CleanupTestAsset(ObjectPath);
    };

    ULevelSequence* Sequence =
        SequencerTestFixtures::MakeTransientSequence(TEXT("AddTrackIdentifierTest"), ObjectPath);
    if (!TestNotNull(TEXT("transient LevelSequence created"), Sequence))
    {
        return true;
    }

    const FString RequestedName = TEXT("PwCameraMove");

    TSharedPtr<FJsonObject> AddPayload = MakeShared<FJsonObject>();
    AddPayload->SetStringField(TEXT("path"), ObjectPath);
    AddPayload->SetStringField(TEXT("trackType"),
        AddTrackIdentifierTestUtils::NameableTrackTypePath());
    AddPayload->SetStringField(TEXT("trackName"), RequestedName);

    FTestResponseCapture AddCapture;
    if (!TestTrue(TEXT("add_track handler found"),
            InvokeHandlerWithCapture(TEXT("sequencer.add_track"), AddPayload, AddCapture)))
    {
        return true;
    }
    if (!TestTrue(TEXT("add_track succeeded on a nameable track class"), AddCapture.bSuccess))
    {
        return true;
    }

    FString ReportedTrackName;
    if (!TestTrue(TEXT("response carries trackName"),
            AddCapture.Result.IsValid()
                && AddCapture.Result->TryGetStringField(TEXT("trackName"), ReportedTrackName)))
    {
        return true;
    }
    FString ReportedDisplayName;
    TestTrue(TEXT("response carries displayName"),
        AddCapture.Result->TryGetStringField(TEXT("displayName"), ReportedDisplayName));

    // The identifier must be measured off the track, not echoed from the request. The engine
    // auto-name is <ClassName>_<n>, never the caller's string.
    TestNotEqual(TEXT("trackName is not the requested name echoed back"),
        ReportedTrackName, RequestedName);
    TestEqual(TEXT("displayName is the requested name"), ReportedDisplayName, RequestedName);

    // Graph truth: the name is on the track, not merely in the response.
    UMovieScene* MovieScene = Sequence->GetMovieScene();
    if (!TestNotNull(TEXT("MovieScene present"), MovieScene))
    {
        return true;
    }
    UMovieSceneTrack* Created = nullptr;
    for (UMovieSceneTrack* Track : MovieScene->GetTracks())
    {
        if (Track && Track->GetName() == ReportedTrackName)
        {
            Created = Track;
            break;
        }
    }
    if (!TestNotNull(TEXT("a track with exactly the reported trackName exists in the MovieScene"),
            Created))
    {
        return true;
    }
    TestEqual(TEXT("the requested name was written to the track's DisplayName"),
        Created->GetDisplayName().ToString(), RequestedName);

    // The whole point: pipe the returned identifier straight back into a consumer verb.
    {
        TSharedPtr<FJsonObject> MutePayload = MakeShared<FJsonObject>();
        MutePayload->SetStringField(TEXT("path"), ObjectPath);
        MutePayload->SetStringField(TEXT("trackName"), ReportedTrackName);
        MutePayload->SetBoolField(TEXT("muted"), true);
        FTestResponseCapture MuteCapture;
        if (TestTrue(TEXT("set_track_muted handler found"),
                InvokeHandlerWithCapture(TEXT("sequencer.set_track_muted"), MutePayload, MuteCapture)))
        {
            TestTrue(TEXT("set_track_muted resolves the trackName add_track returned"),
                MuteCapture.bSuccess);
            TestTrue(TEXT("the track really was muted"), Created->IsEvalDisabled());
        }
    }

    // ...and the display name the caller chose must resolve too, because add_track now writes it.
    {
        TSharedPtr<FJsonObject> LockPayload = MakeShared<FJsonObject>();
        LockPayload->SetStringField(TEXT("path"), ObjectPath);
        LockPayload->SetStringField(TEXT("trackName"), RequestedName);
        LockPayload->SetBoolField(TEXT("locked"), true);
        FTestResponseCapture LockCapture;
        if (TestTrue(TEXT("set_track_locked handler found"),
                InvokeHandlerWithCapture(TEXT("sequencer.set_track_locked"), LockPayload, LockCapture)))
        {
            TestTrue(TEXT("set_track_locked resolves the displayName add_track wrote"),
                LockCapture.bSuccess);
        }
    }

    return true;
}

// ============================================================================
// FAILURE DIRECTION: a trackName that cannot be applied is refused, and nothing is added.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSequencerAddTrackRefusesUnappliableNameTest,
    "PinWright.Sequencer.AddTrackIdentifier.UnnameableTrackTypeRefusesTrackName",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSequencerAddTrackRefusesUnappliableNameTest::RunTest(const FString& Parameters)
{
    FString ObjectPath;
    // The fixture is RF_Standalone, so the periodic suite GC keeps it alive; detach it so
    // it cannot answer a later /Engine/Transient asset-registry rescan.
    ON_SCOPE_EXIT
    {
        CleanupTestAsset(ObjectPath);
    };

    ULevelSequence* Sequence =
        SequencerTestFixtures::MakeTransientSequence(TEXT("AddTrackRefuseNameTest"), ObjectPath);
    if (!TestNotNull(TEXT("transient LevelSequence created"), Sequence))
    {
        return true;
    }
    UMovieScene* MovieScene = Sequence->GetMovieScene();
    if (!TestNotNull(TEXT("MovieScene present"), MovieScene))
    {
        return true;
    }
    const int32 TracksBefore = MovieScene->GetTracks().Num();

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("path"), ObjectPath);
    Payload->SetStringField(TEXT("trackType"),
        AddTrackIdentifierTestUtils::UnnameableTrackTypePath());
    Payload->SetStringField(TEXT("trackName"), TEXT("PwNameThatCannotStick"));

    FTestResponseCapture Capture;
    if (!TestTrue(TEXT("add_track handler found"),
            InvokeHandlerWithCapture(TEXT("sequencer.add_track"), Payload, Capture)))
    {
        return true;
    }

    // The operation could not be carried out as asked, so it must not report success.
    TestFalse(TEXT("add_track refuses a trackName it cannot store"), Capture.bSuccess);
    TestEqual(TEXT("refusal uses the TRACK_NAME_NOT_APPLIED code"),
        Capture.ErrorCode, FString(TEXT("TRACK_NAME_NOT_APPLIED")));
    // Validate-before-mutate: a refused request leaves the sequence exactly as it was.
    TestEqual(TEXT("no track was added by the refused call"),
        MovieScene->GetTracks().Num(), TracksBefore);

    // Control: the same track type WITHOUT a trackName is still accepted, so the refusal is
    // about the unappliable name and not about the track class being unsupported.
    TSharedPtr<FJsonObject> NoNamePayload = MakeShared<FJsonObject>();
    NoNamePayload->SetStringField(TEXT("path"), ObjectPath);
    NoNamePayload->SetStringField(TEXT("trackType"),
        AddTrackIdentifierTestUtils::UnnameableTrackTypePath());
    FTestResponseCapture NoNameCapture;
    if (TestTrue(TEXT("add_track handler found (no-name control)"),
            InvokeHandlerWithCapture(TEXT("sequencer.add_track"), NoNamePayload, NoNameCapture)))
    {
        TestTrue(TEXT("the same track type is accepted without a trackName"),
            NoNameCapture.bSuccess);
        TestEqual(TEXT("the control call did add a track"),
            MovieScene->GetTracks().Num(), TracksBefore + 1);
    }

    return true;
}

// ============================================================================
// FAILURE DIRECTION: an unresolvable trackType says the type did not resolve, and adds nothing.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSequencerAddTrackUnresolvableTypeTest,
    "PinWright.Sequencer.AddTrackIdentifier.UnresolvableTrackTypeAddsNothing",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSequencerAddTrackUnresolvableTypeTest::RunTest(const FString& Parameters)
{
    FString ObjectPath;
    // The fixture is RF_Standalone, so the periodic suite GC keeps it alive; detach it so
    // it cannot answer a later /Engine/Transient asset-registry rescan.
    ON_SCOPE_EXIT
    {
        CleanupTestAsset(ObjectPath);
    };

    ULevelSequence* Sequence =
        SequencerTestFixtures::MakeTransientSequence(TEXT("AddTrackBadTypeTest"), ObjectPath);
    if (!TestNotNull(TEXT("transient LevelSequence created"), Sequence))
    {
        return true;
    }
    UMovieScene* MovieScene = Sequence->GetMovieScene();
    if (!TestNotNull(TEXT("MovieScene present"), MovieScene))
    {
        return true;
    }
    const int32 TracksBefore = MovieScene->GetTracks().Num();

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("path"), ObjectPath);
    Payload->SetStringField(TEXT("trackType"), TEXT("PwDefinitelyNotATrackType"));

    FTestResponseCapture Capture;
    if (!TestTrue(TEXT("add_track handler found"),
            InvokeHandlerWithCapture(TEXT("sequencer.add_track"), Payload, Capture)))
    {
        return true;
    }
    TestFalse(TEXT("add_track fails on an unresolvable trackType"), Capture.bSuccess);
    // The old code answered TRACK_CREATION_FAILED, describing an AddTrack attempt that never
    // happened; nothing was created because the type string never became a class.
    TestEqual(TEXT("the error names the actual cause"),
        Capture.ErrorCode, FString(TEXT("CLASS_NOT_FOUND")));
    TestEqual(TEXT("no track was added"), MovieScene->GetTracks().Num(), TracksBefore);

    return true;
}

// ============================================================================
// The trackName list_tracks reports also filters list_sections. The filter used to compare
// against UMovieSceneTrack::GetTrackName(), which is NAME_None for every track class that does
// not override it, so a trackName piped in from list_tracks or add_track matched nothing and the
// verb reported an EMPTY sections[] for a track that has sections.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSequencerListSectionsTrackNameFilterTest,
    "PinWright.Sequencer.AddTrackIdentifier.ListSectionsFilterAcceptsReportedTrackName",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSequencerListSectionsTrackNameFilterTest::RunTest(const FString& Parameters)
{
    FString ObjectPath;
    // The fixture is RF_Standalone, so the periodic suite GC keeps it alive; detach it so
    // it cannot answer a later /Engine/Transient asset-registry rescan.
    ON_SCOPE_EXIT
    {
        CleanupTestAsset(ObjectPath);
    };

    ULevelSequence* Sequence =
        SequencerTestFixtures::MakeTransientSequence(TEXT("ListSectionsFilterTest"), ObjectPath);
    if (!TestNotNull(TEXT("transient LevelSequence created"), Sequence))
    {
        return true;
    }

    // Build the track through the production verb so the identifier under test is the one
    // add_track actually hands out.
    TSharedPtr<FJsonObject> AddPayload = MakeShared<FJsonObject>();
    AddPayload->SetStringField(TEXT("path"), ObjectPath);
    AddPayload->SetStringField(TEXT("trackType"),
        AddTrackIdentifierTestUtils::NameableTrackTypePath());
    FTestResponseCapture AddCapture;
    if (!TestTrue(TEXT("add_track handler found"),
            InvokeHandlerWithCapture(TEXT("sequencer.add_track"), AddPayload, AddCapture)))
    {
        return true;
    }
    FString ReportedTrackName;
    if (!TestTrue(TEXT("add_track reported a trackName"),
            AddCapture.bSuccess && AddCapture.Result.IsValid()
                && AddCapture.Result->TryGetStringField(TEXT("trackName"), ReportedTrackName)))
    {
        return true;
    }

    // Give it a section through the production verb too.
    TSharedPtr<FJsonObject> SectionPayload = MakeShared<FJsonObject>();
    SectionPayload->SetStringField(TEXT("path"), ObjectPath);
    SectionPayload->SetStringField(TEXT("trackName"), ReportedTrackName);
    SectionPayload->SetNumberField(TEXT("startFrame"), 0.0);
    SectionPayload->SetNumberField(TEXT("endFrame"), 100.0);
    FTestResponseCapture SectionCapture;
    if (!TestTrue(TEXT("add_section handler found"),
            InvokeHandlerWithCapture(TEXT("sequencer.add_section"), SectionPayload, SectionCapture)))
    {
        return true;
    }
    if (!TestTrue(TEXT("add_section resolves the trackName add_track returned"),
            SectionCapture.bSuccess))
    {
        return true;
    }

    // Unfiltered: the section is there.
    {
        TSharedPtr<FJsonObject> ListPayload = MakeShared<FJsonObject>();
        ListPayload->SetStringField(TEXT("path"), ObjectPath);
        FTestResponseCapture ListCapture;
        if (TestTrue(TEXT("list_sections handler found (unfiltered)"),
                InvokeHandlerWithCapture(TEXT("sequencer.list_sections"), ListPayload, ListCapture)))
        {
            TestTrue(TEXT("list_sections succeeded (unfiltered)"), ListCapture.bSuccess);
            const TArray<TSharedPtr<FJsonValue>>* Sections = nullptr;
            if (TestTrue(TEXT("sections[] present (unfiltered)"),
                    ListCapture.Result.IsValid()
                        && ListCapture.Result->TryGetArrayField(TEXT("sections"), Sections)))
            {
                TestEqual(TEXT("one section listed (unfiltered)"), Sections->Num(), 1);
            }
        }
    }

    // Filtered by the very identifier the other verbs report: must NOT come back empty.
    {
        TSharedPtr<FJsonObject> ListPayload = MakeShared<FJsonObject>();
        ListPayload->SetStringField(TEXT("path"), ObjectPath);
        ListPayload->SetStringField(TEXT("trackName"), ReportedTrackName);
        FTestResponseCapture ListCapture;
        if (TestTrue(TEXT("list_sections handler found (filtered)"),
                InvokeHandlerWithCapture(TEXT("sequencer.list_sections"), ListPayload, ListCapture)))
        {
            TestTrue(TEXT("list_sections succeeded (filtered)"), ListCapture.bSuccess);
            const TArray<TSharedPtr<FJsonValue>>* Sections = nullptr;
            if (TestTrue(TEXT("sections[] present (filtered)"),
                    ListCapture.Result.IsValid()
                        && ListCapture.Result->TryGetArrayField(TEXT("sections"), Sections)))
            {
                TestEqual(TEXT("the trackName filter matches the reported identifier"),
                    Sections->Num(), 1);
            }
            // And the per-section trackName must be the same identifier, so a filter built from
            // a previous list_sections response also resolves.
            TSharedPtr<FJsonObject> First;
            if (Sections && Sections->Num() > 0 && (*Sections)[0].IsValid())
            {
                First = (*Sections)[0]->AsObject();
            }
            if (First.IsValid())
            {
                FString SectionTrackName;
                TestTrue(TEXT("section entry carries trackName"),
                    First->TryGetStringField(TEXT("trackName"), SectionTrackName));
                TestEqual(TEXT("section trackName is the same identifier"),
                    SectionTrackName, ReportedTrackName);
            }
        }
    }

    return true;
}

// ============================================================================
// sequencer.add_section takes startFrame/endFrame as DISPLAY-rate frame numbers, but
// UMovieSceneSection::SetRange stores TICK-resolution frame numbers. The handler used to
// hand the raw request values straight to SetRange, so at the default 24 fps display rate
// over a 24000-tick resolution the documented endFrame:100 authored a section spanning
// 100 ticks (0.0042 s) instead of 100000 ticks (~4.17 s) -- a ~1000x error reported as
// success:true, with the response echoing the caller's own startFrame/endFrame back, so no
// response-level check could see it. Same defect family as E-sequencer-property-unit-drift
// (sequencer.set_properties) and B-sequencer-section-range-display-frames-as-ticks
// (sequencer.add_camera_track); the fix routes both bounds through
// SequenceHelpers::DisplayFrameToTick like every other write path in SequenceHandler.cpp.
//
// The expected ticks are re-derived from the MovieScene's OWN display rate and tick
// resolution, not from a re-implementation of the handler's conversion. Counterfactual:
// revert add_section to the bare FFrameNumber cast and the stored range becomes [10, 100),
// failing every assertion below.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSequencerAddSectionRangeUnitsTest,
    "PinWright.Sequencer.AddTrackIdentifier.AddSectionRangeIsDisplayFramesInTicks",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSequencerAddSectionRangeUnitsTest::RunTest(const FString& Parameters)
{
    FString ObjectPath;
    // The fixture is RF_Standalone, so the periodic suite GC keeps it alive; detach it so
    // it cannot answer a later /Engine/Transient asset-registry rescan.
    ON_SCOPE_EXIT
    {
        CleanupTestAsset(ObjectPath);
    };

    ULevelSequence* Sequence =
        SequencerTestFixtures::MakeTransientSequence(TEXT("AddSectionRangeUnitsTest"), ObjectPath);
    if (!TestNotNull(TEXT("transient LevelSequence created"), Sequence))
    {
        return true;
    }
    UMovieScene* MovieScene = Sequence->GetMovieScene();
    if (!TestNotNull(TEXT("MovieScene present"), MovieScene))
    {
        return true;
    }

    // Pin a display rate distinct from the tick resolution so display frames and ticks cannot
    // coincide: 24 fps over 24000 ticks = 1000 ticks per displayed frame.
    MovieScene->SetTickResolutionDirectly(FFrameRate(24000, 1));
    MovieScene->SetDisplayRate(FFrameRate(24, 1));

    // Non-zero start so BOTH bounds discriminate (frame 0 converts to tick 0 either way).
    const double StartFrameInput = 10.0;   // frame 10  ->  10000 ticks
    const double EndFrameInput = 100.0;    // frame 100 -> 100000 ticks (the documented default)

    const FFrameNumber ExpectedStartTicks =
        AddTrackIdentifierTestUtils::ExpectedSectionTick(MovieScene, StartFrameInput);
    const FFrameNumber ExpectedEndTicks =
        AddTrackIdentifierTestUtils::ExpectedSectionTick(MovieScene, EndFrameInput);

    // Precondition: if display frames and ticks ever coincide the assertions below become
    // degenerate and would pass against the defect. Fail loudly instead of silently weakening.
    if (!TestTrue(TEXT("fixture discriminates: the tick conversion differs from the raw display "
                       "frame for both bounds (else display frames == ticks and the units check "
                       "proves nothing)"),
            ExpectedStartTicks.Value != static_cast<int32>(StartFrameInput)
                && ExpectedEndTicks.Value != static_cast<int32>(EndFrameInput)))
    {
        return true;
    }

    // Build the track through the production verb so add_section resolves the identifier it is
    // actually handed in practice.
    TSharedPtr<FJsonObject> AddPayload = MakeShared<FJsonObject>();
    AddPayload->SetStringField(TEXT("path"), ObjectPath);
    AddPayload->SetStringField(TEXT("trackType"),
        AddTrackIdentifierTestUtils::NameableTrackTypePath());
    FTestResponseCapture AddCapture;
    if (!TestTrue(TEXT("add_track handler found"),
            InvokeHandlerWithCapture(TEXT("sequencer.add_track"), AddPayload, AddCapture)))
    {
        return true;
    }
    FString ReportedTrackName;
    if (!TestTrue(TEXT("add_track reported a trackName"),
            AddCapture.bSuccess && AddCapture.Result.IsValid()
                && AddCapture.Result->TryGetStringField(TEXT("trackName"), ReportedTrackName)))
    {
        return true;
    }

    TSharedPtr<FJsonObject> SectionPayload = MakeShared<FJsonObject>();
    SectionPayload->SetStringField(TEXT("path"), ObjectPath);
    SectionPayload->SetStringField(TEXT("trackName"), ReportedTrackName);
    SectionPayload->SetNumberField(TEXT("startFrame"), StartFrameInput);
    SectionPayload->SetNumberField(TEXT("endFrame"), EndFrameInput);
    FTestResponseCapture SectionCapture;
    if (!TestTrue(TEXT("add_section handler found"),
            InvokeHandlerWithCapture(TEXT("sequencer.add_section"), SectionPayload, SectionCapture)))
    {
        return true;
    }
    // The handler answers success:true both before and after the fix -- recorded as a
    // precondition documenting the false success, not as the thing under test.
    if (!TestTrue(TEXT("add_section reported success"), SectionCapture.bSuccess))
    {
        return true;
    }

    // Graph truth: read the range off the section the handler authored.
    UMovieSceneTrack* Created = nullptr;
    for (UMovieSceneTrack* Track : MovieScene->GetTracks())
    {
        if (Track && Track->GetName() == ReportedTrackName)
        {
            Created = Track;
            break;
        }
    }
    if (!TestNotNull(TEXT("the track add_section wrote into exists in the MovieScene"), Created))
    {
        return true;
    }
    const TArray<UMovieSceneSection*>& Sections = Created->GetAllSections();
    if (!TestEqual(TEXT("add_section authored exactly one section"), Sections.Num(), 1))
    {
        return true;
    }
    UMovieSceneSection* Section = Sections[0];
    if (!TestNotNull(TEXT("authored section present"), Section))
    {
        return true;
    }
    if (!TestTrue(TEXT("authored section has a bounded start and end"),
            Section->HasStartFrame() && Section->HasEndFrame()))
    {
        return true;
    }

    const FFrameNumber ActualStart = Section->GetInclusiveStartFrame();
    const FFrameNumber ActualEnd = Section->GetExclusiveEndFrame();

    AddInfo(FString::Printf(
        TEXT("add_section stored range [%d, %d) ticks @ DisplayRate %d/%d, TickResolution %d/%d "
             "(requested display frames %.0f..%.0f; display-frames-as-ticks bug stores [%d, %d))"),
        ActualStart.Value, ActualEnd.Value,
        MovieScene->GetDisplayRate().Numerator, MovieScene->GetDisplayRate().Denominator,
        MovieScene->GetTickResolution().Numerator, MovieScene->GetTickResolution().Denominator,
        StartFrameInput, EndFrameInput,
        static_cast<int32>(StartFrameInput), static_cast<int32>(EndFrameInput)));

    TestEqual(TEXT("section start is the display-frame -> tick conversion (10 -> 10000)"),
        ActualStart.Value, ExpectedStartTicks.Value);
    TestEqual(TEXT("section end is the display-frame -> tick conversion (100 -> 100000)"),
        ActualEnd.Value, ExpectedEndTicks.Value);

    // Anti-regression: the bounds must not be the raw, unconverted display frames the pre-fix
    // bare cast stored (a ~1000x-too-short section reported as success).
    TestNotEqual(TEXT("section start is not the unconverted raw display frame (10)"),
        ActualStart.Value, static_cast<int32>(StartFrameInput));
    TestNotEqual(TEXT("section end is not the unconverted raw display frame (100)"),
        ActualEnd.Value, static_cast<int32>(EndFrameInput));

    // Duration sanity in the unit a caller reasons in: 90 display frames at 24 fps = 3.75 s.
    const double ActualDurationSeconds =
        MovieScene->GetTickResolution().AsSeconds(FFrameTime(ActualEnd - ActualStart));
    const double ExpectedDurationSeconds =
        MovieScene->GetDisplayRate().AsSeconds(FFrameTime(FFrameNumber(
            static_cast<int32>(EndFrameInput) - static_cast<int32>(StartFrameInput))));
    TestTrue(FString::Printf(TEXT("section spans the requested display-frame count in seconds "
                                  "(expected %.4f s, got %.4f s)"),
                 ExpectedDurationSeconds, ActualDurationSeconds),
        FMath::IsNearlyEqual(ActualDurationSeconds, ExpectedDurationSeconds, 0.001));

    return true;
}
