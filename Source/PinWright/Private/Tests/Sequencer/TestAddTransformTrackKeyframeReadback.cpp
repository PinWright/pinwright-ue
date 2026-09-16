// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for E-sequencer-add-transform-track-phantom-default-keyframes.
//
// sequencer.add_transform_track adds a UMovieScene3DTransformTrack and a fresh
// UMovieScene3DTransformSection (AddTrack -> CreateNewSection() -> AddSection()) but
// never seeds a keyframe. Before the fix its success payload hardcoded
// hasDefaultKeyframes:true unconditionally, which lied about a section it had just
// created empty (a zero-length [0,0] range with keyCount:0 on every channel). A caller
// trusting the field could conclude the track was already keyed and skip authoring a
// rest pose.
//
// The fix derives the field from the section's REAL key count via
// MovieSceneJsonUtils::CountSectionKeys (the same GetAllEntries()/GetNumKeys() walk the
// list_sections{includeKeys} readback uses) and echoes that keyCount. This test drives
// the REAL registered sequencer.add_transform_track handler through
// InvokeHandlerWithCapture (production code, not a copy) on an in-code transient
// LevelSequence with a bound possessable, then asserts the response's keyCount/
// hasDefaultKeyframes match the section's actual (zero) key state. Counterfactual:
// revert to the hardcoded hasDefaultKeyframes:true and the false-assertion fails; drop
// the keyCount echo and the presence assertion fails.
#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Tests/TestUtils.h"
#include "Tests/Sequencer/SequencerTestFixtures.h"
#include "Utils/MovieSceneJsonUtils.h"

#include "LevelSequence.h"
#include "MovieScene.h"
#include "Tracks/MovieScene3DTransformTrack.h"
#include "Sections/MovieScene3DTransformSection.h"
#include "GameFramework/Actor.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAddTransformTrackNoPhantomKeyframesTest,
    "PinWright.Sequencer.AddTransformTrack.NoPhantomDefaultKeyframes",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAddTransformTrackNoPhantomKeyframesTest::RunTest(const FString& Parameters)
{
    TestTrue(TEXT("sequencer.add_transform_track handler registered"),
        IsHandlerRegistered(TEXT("sequencer.add_transform_track")));

    // In-code fixture: a transient LevelSequence the load-based handler resolves by object
    // path (same path the passing add_level_visibility_track test drives). A null here is a
    // real environment failure, not a skippable missing asset — TestNotNull fails the test.
    FString ObjectPath;
    // The fixture is RF_Standalone, so the periodic suite GC keeps it alive; detach it so
    // it cannot answer a later /Engine/Transient asset-registry rescan.
    ON_SCOPE_EXIT
    {
        CleanupTestAsset(ObjectPath);
    };

    ULevelSequence* Sequence = SequencerTestFixtures::MakeTransientSequence(
        TEXT("AddTransformTrackReadback"), ObjectPath);
    if (!TestNotNull(TEXT("transient LevelSequence created"), Sequence))
    {
        return true;
    }
    UMovieScene* MovieScene = Sequence->GetMovieScene();
    if (!TestNotNull(TEXT("MovieScene present"), MovieScene))
    {
        return true;
    }

    const FGuid BindingGuid = MovieScene->AddPossessable(TEXT("ProbeActor"), AActor::StaticClass());
    if (!TestTrue(TEXT("binding GUID is valid"), BindingGuid.IsValid()))
    {
        return true;
    }

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("sequencePath"), ObjectPath);
    Payload->SetStringField(TEXT("bindingGuid"), BindingGuid.ToString(EGuidFormats::Digits));

    FTestResponseCapture Capture;
    TestTrue(TEXT("sequencer.add_transform_track handler invoked"),
        InvokeHandlerWithCapture(TEXT("sequencer.add_transform_track"), Payload, Capture));
    TestTrue(TEXT("handler responded"), Capture.bWasCalled);
    TestTrue(TEXT("handler reported success"), Capture.bSuccess);
    if (!Capture.bSuccess || !Capture.Result.IsValid())
    {
        return true;
    }

    // Ground truth: the section the handler just created really has zero keys. Re-read it
    // from the MovieScene independently and count its keys via the same production helper the
    // handler uses, so the assertions below check the payload against the section's real state.
    UMovieScene3DTransformTrack* Track =
        MovieScene->FindTrack<UMovieScene3DTransformTrack>(BindingGuid);
    if (!TestNotNull(TEXT("transform track created on the binding"), Track))
    {
        return true;
    }
    const TArray<UMovieSceneSection*>& Sections = Track->GetAllSections();
    if (!TestEqual(TEXT("exactly one section on the transform track"), Sections.Num(), 1))
    {
        return true;
    }
    UMovieScene3DTransformSection* Section = Cast<UMovieScene3DTransformSection>(Sections[0]);
    if (!TestNotNull(TEXT("section is a UMovieScene3DTransformSection"), Section))
    {
        return true;
    }
    const int32 RealKeyCount = MovieSceneJsonUtils::CountSectionKeys(Section);
    TestEqual(TEXT("freshly created section has no keys"), RealKeyCount, 0);

    // Core regression assertions: the payload must reflect the section's real key state, not a
    // hardcoded constant. Pre-fix hasDefaultKeyframes was hardcoded true (fails the false check)
    // and there was no keyCount field at all (fails the presence check).
    bool bHasDefaultKeyframes = true; // default true so an absent field still trips the check
    TestTrue(TEXT("response carries hasDefaultKeyframes"),
        Capture.Result->TryGetBoolField(TEXT("hasDefaultKeyframes"), bHasDefaultKeyframes));
    TestEqual(TEXT("hasDefaultKeyframes reflects the real key count (false for an empty section)"),
        bHasDefaultKeyframes, RealKeyCount > 0);
    TestFalse(TEXT("empty section reports no default keyframes"), bHasDefaultKeyframes);

    double EchoedKeyCount = -1.0;
    TestTrue(TEXT("response carries keyCount"),
        Capture.Result->TryGetNumberField(TEXT("keyCount"), EchoedKeyCount));
    TestEqual(TEXT("echoed keyCount equals the section's real key count"),
        static_cast<int32>(EchoedKeyCount), RealKeyCount);

    return true;
}
