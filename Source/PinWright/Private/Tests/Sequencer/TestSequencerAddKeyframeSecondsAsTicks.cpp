// Copyright (c) 2026 Alexander Penkin. MIT License.

// Red test for B-sequencer-add-keyframe-seconds-as-ticks.
//
// sequencer.add_keyframe (the modern seconds-based verb, distinct from the legacy
// frame-numbered sequence.add_keyframe) takes a keyframe `time` in SECONDS and writes a
// cubic key onto a float channel. It converts the seconds to DISPLAY-RATE frames
// (FFrameRate::AsFrameTime(Seconds).GetFrame()) and hands that frame number straight to
// FMovieSceneFloatChannel::AddCubicKey — whose key times are indexed in the MovieScene's
// TICK RESOLUTION, not its DisplayRate. At the default 24 fps DisplayRate / 24000
// TickResolution the two units differ by 1000x, so a key requested at 5 s is stored at
// tick 120 (0.005 s) instead of tick 120000 (5 s) — ~1000x too early — while the RPC
// still reports success and echoes the requested seconds (SequencerHandler.cpp:150-154,162).
//
// This test drives the PRODUCTION sequencer.add_keyframe handler through the real
// registration list (InvokeHandlerWithCapture), then reads back the float channel's single
// stored key time and asserts the CORRECT behavior: the stored key frame must be the
// requested seconds expressed in TICK resolution (TickResolution.AsFrameNumber(5) == 120000
// at defaults), NOT the DisplayRate-frame value (120). The expected frame is re-derived
// from the MovieScene's own GetTickResolution() (the spec — "seconds in tick-resolution
// frames"), not from a re-implementation of the handler's buggy conversion.
//
// Fixture is content-free: a real /Game LevelSequence created via the registered
// sequencer.create handler (so add_keyframe's LoadObject<ULevelSequence> resolves it) with a
// possessable binding added in-code (add_keyframe only needs FindBinding to succeed; the
// float track/section it authors does not require a bound object instance).
//
// Differential property: pre-fix AddCubicKey receives DisplayRate frame 120, so the stored
// key lands at tick 120 (0.005 s) and both the tick-equality and the ~5 s round-trip
// assertions FAIL, reproducing the defect. Once the handler converts seconds to
// tick-resolution frames (MovieScene->GetTickResolution().AsFrameNumber(Seconds)), the key
// lands at tick 120000 / 5 s and the assertions flip green. success:true is asserted as a
// precondition (the handler reports success both pre- and post-fix — it documents the
// false-success context); the stored-key-time assertions distinguish fixed from broken.
#include "Misc/AutomationTest.h"

#include "Dom/JsonObject.h"
#include "EditorAssetLibrary.h"
#include "Misc/Guid.h"
#include "Misc/ScopeExit.h"

#include "GameFramework/Actor.h"
#include "LevelSequence.h"
#include "MovieScene.h"
#include "MovieSceneBinding.h"
#include "Tracks/MovieSceneFloatTrack.h"
#include "Sections/MovieSceneFloatSection.h"
#include "Channels/MovieSceneFloatChannel.h"

#include "Tests/TestUtils.h"

namespace
{
    // Distinctly named (mirrors CreateSectionRangeUnitsSequence / CreateProbeSequence in
    // sibling sequencer test .cpp files) so anonymous-namespace symbols don't ODR-collide
    // when Unity merges these TUs: create a real /Game LevelSequence via the registered
    // sequencer.create handler so add_keyframe's LoadObject<ULevelSequence> can resolve it.
    // Empty path + nullptr on failure.
    ULevelSequence* CreateAddKeyframeUnitsSequence(FAutomationTestBase& Test, FString& OutFullPath)
    {
        const FString SeqName = FString::Printf(TEXT("MCP_AddKeyframeUnitsSeq_%s"),
            *FGuid::NewGuid().ToString(EGuidFormats::Digits));
        const FString DestFolder = TEXT("/Game/MCP_AddKeyframeUnitsProbe");
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
                               "the add_keyframe seconds-as-ticks repro cannot be exercised without it."));
            OutFullPath.Reset();
            return nullptr;
        }
        return Cast<ULevelSequence>(UEditorAssetLibrary::LoadAsset(OutFullPath));
    }

    // Reads the single stored key time on the binding's float track for PropertyName, mirroring
    // the handler's OWN track lookup (Cast<UMovieSceneFloatTrack> + case-insensitive
    // GetPropertyName match) so the exact channel the handler wrote is the one inspected.
    // Returns true and fills OutTime only when the channel holds exactly one key.
    bool GetSingleFloatKeyTime(UMovieScene* MovieScene, const FGuid& BindingGuid,
        const FString& PropertyName, FFrameNumber& OutTime)
    {
        FMovieSceneBinding* Binding = MovieScene ? MovieScene->FindBinding(BindingGuid) : nullptr;
        if (!Binding)
        {
            return false;
        }
        for (UMovieSceneTrack* T : Binding->GetTracks())
        {
            UMovieSceneFloatTrack* FT = Cast<UMovieSceneFloatTrack>(T);
            if (!FT || !FT->GetPropertyName().ToString().Equals(PropertyName, ESearchCase::IgnoreCase))
            {
                continue;
            }
            const TArray<UMovieSceneSection*>& Sections = FT->GetAllSections();
            if (Sections.Num() == 0)
            {
                return false;
            }
            UMovieSceneFloatSection* FloatSection = Cast<UMovieSceneFloatSection>(Sections[0]);
            if (!FloatSection)
            {
                return false;
            }
            const TArrayView<const FFrameNumber> Times = FloatSection->GetChannel().GetData().GetTimes();
            if (Times.Num() != 1)
            {
                return false;
            }
            OutTime = Times[0];
            return true;
        }
        return false;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSequencerAddKeyframeSecondsAsTicksTest,
    "PinWright.Sequencer.AddKeyframe.SecondsStoredAsTickFrame",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSequencerAddKeyframeSecondsAsTicksTest::RunTest(const FString& Parameters)
{
    // A real /Game LevelSequence for add_keyframe to author a float key into.
    FString FullPath;
    ULevelSequence* Sequence = CreateAddKeyframeUnitsSequence(*this, FullPath);
    if (!Sequence)
    {
        return false; // error already emitted by helper
    }
    ON_SCOPE_EXIT { CleanupTestAsset(FullPath); };

    UMovieScene* MovieScene = Sequence->GetMovieScene();
    if (!TestNotNull(TEXT("MovieScene present on the probe sequence"), MovieScene))
    {
        return false;
    }

    const FFrameRate TickResolution = MovieScene->GetTickResolution();
    const FFrameRate DisplayRate    = MovieScene->GetDisplayRate();

    const double KeyTimeSeconds = 5.0;
    // The correct stored key frame for a 5 s key: seconds in TICK-resolution frames.
    const FFrameNumber ExpectedTicks = TickResolution.AsFrameNumber(KeyTimeSeconds);
    // What the defect produces: seconds in DISPLAY-rate frames fed to AddCubicKey.
    const FFrameNumber DisplayFrame  = DisplayRate.AsFrameTime(KeyTimeSeconds).GetFrame();

    // Precondition: the two units must differ, or this fixture cannot demonstrate the bug.
    // At the default 24000 TickResolution / 24 fps DisplayRate they differ 1000x.
    if (!TestTrue(TEXT("fixture exposes the defect: TickResolution frame != DisplayRate frame "
                       "for the requested seconds (else units coincide and the bug is invisible)"),
            ExpectedTicks != DisplayFrame))
    {
        return false;
    }

    // A possessable binding so add_keyframe's FindBinding resolves. The handler needs only a
    // valid binding GUID; the float track/section it authors doesn't require a bound object.
    const FGuid BindingGuid =
        MovieScene->AddPossessable(TEXT("MCP_AddKeyframeProbeActor"), AActor::StaticClass());
    if (!TestTrue(TEXT("possessable binding GUID is valid"), BindingGuid.IsValid()))
    {
        return false;
    }

    const FString PropertyName = TEXT("Intensity");

    // Drive the production sequencer.add_keyframe handler: a float key at time=5 s.
    TSharedPtr<FJsonObject> AddPayload = MakeShared<FJsonObject>();
    AddPayload->SetStringField(TEXT("sequencePath"), FullPath);
    AddPayload->SetStringField(TEXT("bindingGuid"), BindingGuid.ToString());
    AddPayload->SetStringField(TEXT("propertyName"), PropertyName);
    AddPayload->SetNumberField(TEXT("time"), KeyTimeSeconds);
    AddPayload->SetNumberField(TEXT("value"), 2.0);

    FTestResponseCapture AddCapture;
    const bool bFound = InvokeHandlerWithCapture(TEXT("sequencer.add_keyframe"), AddPayload, AddCapture);
    if (!TestTrue(TEXT("sequencer.add_keyframe handler is registered and invoked"), bFound))
    {
        return false;
    }
    // Preconditions documenting the false success: the handler reports success:true both
    // pre- and post-fix. A failure here is a fixture problem, not the units defect.
    if (!TestTrue(TEXT("sequencer.add_keyframe responded"), AddCapture.bWasCalled))
    {
        return false;
    }
    if (!TestTrue(TEXT("sequencer.add_keyframe reported success"), AddCapture.bSuccess))
    {
        AddError(FString::Printf(TEXT("add_keyframe failed (code=%s msg=%s) — fixture problem, "
                                      "not the seconds-as-ticks defect"), *AddCapture.ErrorCode, *AddCapture.Message));
        return false;
    }

    // Read back the single key the handler authored on the float channel.
    FFrameNumber StoredTime;
    if (!TestTrue(TEXT("float track/section/channel has exactly one authored key"),
            GetSingleFloatKeyTime(MovieScene, BindingGuid, PropertyName, StoredTime)))
    {
        return false;
    }

    const double StoredSeconds = TickResolution.AsSeconds(FFrameTime(StoredTime));
    AddInfo(FString::Printf(
        TEXT("add_keyframe stored key at tick %d @ TickResolution %d/%d -> %.6f s "
             "(requested %.1f s; DisplayRate-frames-as-ticks bug would store tick %d)"),
        StoredTime.Value, TickResolution.Numerator, TickResolution.Denominator,
        StoredSeconds, KeyTimeSeconds, DisplayFrame.Value));

    // CORRECT-BEHAVIOR ASSERTIONS (the defect surfaces here).
    // 1) The stored key frame must be the requested seconds in TICK resolution (120000 at
    //    defaults), NOT the DisplayRate-frame value (120) fed to AddCubicKey. Pre-fix == 120.
    TestEqual(TEXT("stored key frame is the requested seconds in TICK resolution "
                   "(not DisplayRate frames fed to AddCubicKey)"),
        StoredTime.Value, ExpectedTicks.Value);

    // 2) Converting the stored key time back to seconds must recover the requested time.
    //    Pre-fix the key sits at ~0.005 s (1000x too early) and this fails.
    TestTrue(FString::Printf(
                 TEXT("stored key time recovers the requested %.1f s when converted back to "
                      "seconds (got %.6f s)"), KeyTimeSeconds, StoredSeconds),
        FMath::IsNearlyEqual(StoredSeconds, KeyTimeSeconds, 0.05));

    return true;
}
