// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for F-sequencer-transform-section-range-not-expanded-by-keyframe.
//
// sequence.add_keyframe on a transform track writes keys onto the section's
// double channels but, before the fix, never grew the section's frame range to
// span those keys. SequenceHelpers::GetOrAddTransformChannels does
// Track->FindOrAddSection(0, ...), which creates a UMovieScene3DTransformSection
// collapsed to [0,0]; both the property="Transform" branch and the per-axis
// Location/Rotation/Scale branch only mutated channel data + MovieScene->Modify()
// and never called Section->SetRange / ExpandToFrame. So a key authored at frame
// 120 landed OUTSIDE the [0,0] section and the animation evaluated over no time
// despite the handler reporting ok=true.
//
// The fix expands the section to each written key (Section->ExpandToFrame at
// each keyframe-write branch). These tests drive the REAL
// registered sequence.add_keyframe handler through InvokeHandlerWithCapture
// (production code, not a copy) on a real LevelSequence asset with a bound
// possessable, then assert the binding's transform section range CONTAINS the
// written key's tick frame. Counterfactual: revert the ExpandToFrame calls and
// the section stays [0,0], the contains check fails, and these tests fail.
#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "EditorAssetLibrary.h"
#include "Tests/TestSkipReporting.h"
#include "Tests/TestUtils.h"

#include "LevelSequence.h"
#include "MovieScene.h"
#include "MovieSceneBinding.h"
#include "Tracks/MovieScene3DTransformTrack.h"
#include "Sections/MovieScene3DTransformSection.h"

namespace
{
    // Create a real /Game LevelSequence via the registered sequencer.create handler
    // (so sequence.add_keyframe's UEditorAssetLibrary::LoadAsset can resolve it),
    // returns its full object path and the loaded sequence. Empty path on failure.
    ULevelSequence* CreateProbeSequence(FAutomationTestBase& Test, FString& OutFullPath)
    {
        const FString SeqName = FString::Printf(TEXT("MCP_KeyframeExpandSeq_%s"),
            *FGuid::NewGuid().ToString(EGuidFormats::Digits));
        const FString DestFolder = TEXT("/Game/MCP_KeyframeExpandProbe");
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
                     "host); skipping transform-section-range assertion"));
            OutFullPath.Reset();
            return nullptr;
        }
        return Cast<ULevelSequence>(UEditorAssetLibrary::LoadAsset(OutFullPath));
    }

    // Returns the single UMovieScene3DTransformSection on the binding's transform
    // track, or nullptr.
    UMovieScene3DTransformSection* FindTransformSection(UMovieScene* MovieScene, const FGuid& BindingGuid)
    {
        if (!MovieScene) return nullptr;
        UMovieScene3DTransformTrack* Track =
            MovieScene->FindTrack<UMovieScene3DTransformTrack>(BindingGuid, FName("Transform"));
        if (!Track) return nullptr;
        const TArray<UMovieSceneSection*>& Sections = Track->GetAllSections();
        return Sections.Num() > 0 ? Cast<UMovieScene3DTransformSection>(Sections[0]) : nullptr;
    }

    // The tick frame the handler writes for a display-rate Frame, mirroring
    // SequenceHelpers::DisplayFrameToTick (FloorToFrame at tick resolution).
    FFrameNumber DisplayFrameToTick(UMovieScene* MovieScene, double Frame)
    {
        const FFrameNumber FrameNum = FFrameNumber(static_cast<int32>(Frame));
        return FFrameRate::TransformTime(
                   FFrameTime(FrameNum), MovieScene->GetDisplayRate(), MovieScene->GetTickResolution())
            .FloorToFrame();
    }
}

// ---- property="Transform" branch: section grows to span the authored key ----
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAddKeyframeTransformExpandsSectionTest,
    "PinWright.Sequencer.AddKeyframe.TransformExpandsSection",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAddKeyframeTransformExpandsSectionTest::RunTest(const FString& Parameters)
{
    TestTrue(TEXT("sequence.add_keyframe handler registered"),
        IsHandlerRegistered(TEXT("sequence.add_keyframe")));

    FString FullPath;
    ULevelSequence* Sequence = CreateProbeSequence(*this, FullPath);
    if (!Sequence)
    {
        return true; // warning already emitted by helper
    }
    UMovieScene* MovieScene = Sequence->GetMovieScene();
    if (!TestNotNull(TEXT("MovieScene present"), MovieScene))
    {
        CleanupTestAsset(FullPath);
        return true;
    }

    // Bind a possessable so add_keyframe can resolve the binding by GUID.
    const FGuid BindingGuid = MovieScene->AddPossessable(TEXT("ProbeActor"), AActor::StaticClass());
    if (!TestTrue(TEXT("binding GUID is valid"), BindingGuid.IsValid()))
    {
        CleanupTestAsset(FullPath);
        return true;
    }

    const double KeyFrame = 120.0;
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("path"), FullPath);
    Payload->SetStringField(TEXT("bindingId"), BindingGuid.ToString(EGuidFormats::Digits));
    Payload->SetStringField(TEXT("property"), TEXT("Transform"));
    Payload->SetNumberField(TEXT("frame"), KeyFrame);
    {
        TSharedPtr<FJsonObject> Value = MakeShared<FJsonObject>();
        TSharedPtr<FJsonObject> Loc = MakeShared<FJsonObject>();
        Loc->SetNumberField(TEXT("x"), 500.0);
        Loc->SetNumberField(TEXT("y"), 0.0);
        Loc->SetNumberField(TEXT("z"), 0.0);
        Value->SetObjectField(TEXT("location"), Loc);
        Payload->SetObjectField(TEXT("value"), Value);
    }

    FTestResponseCapture Capture;
    TestTrue(TEXT("sequence.add_keyframe handler invoked"),
        InvokeHandlerWithCapture(TEXT("sequence.add_keyframe"), Payload, Capture));
    TestTrue(TEXT("handler responded"), Capture.bWasCalled);
    TestTrue(TEXT("handler reported success"), Capture.bSuccess);

    UMovieScene3DTransformSection* Section = FindTransformSection(MovieScene, BindingGuid);
    if (!TestNotNull(TEXT("transform section created"), Section))
    {
        CleanupTestAsset(FullPath);
        return true;
    }

    const FFrameNumber KeyTick = DisplayFrameToTick(MovieScene, KeyFrame);
    // Core regression assertion: the section range must span the authored key.
    // Before the fix the section stays collapsed at [0,0] and does NOT contain
    // the frame-120 tick, so this fails.
    TestTrue(TEXT("section range contains the authored key tick (section expanded)"),
        Section->GetRange().Contains(KeyTick));

    CleanupTestAsset(FullPath);
    return true;
}

// ---- per-axis property="Location" branch: same auto-expand on keyframe write ----
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAddKeyframeLocationExpandsSectionTest,
    "PinWright.Sequencer.AddKeyframe.LocationExpandsSection",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAddKeyframeLocationExpandsSectionTest::RunTest(const FString& Parameters)
{
    FString FullPath;
    ULevelSequence* Sequence = CreateProbeSequence(*this, FullPath);
    if (!Sequence)
    {
        return true;
    }
    UMovieScene* MovieScene = Sequence->GetMovieScene();
    if (!TestNotNull(TEXT("MovieScene present"), MovieScene))
    {
        CleanupTestAsset(FullPath);
        return true;
    }

    const FGuid BindingGuid = MovieScene->AddPossessable(TEXT("ProbeActor"), AActor::StaticClass());
    if (!TestTrue(TEXT("binding GUID is valid"), BindingGuid.IsValid()))
    {
        CleanupTestAsset(FullPath);
        return true;
    }

    const double KeyFrame = 150.0;
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("path"), FullPath);
    Payload->SetStringField(TEXT("bindingId"), BindingGuid.ToString(EGuidFormats::Digits));
    Payload->SetStringField(TEXT("property"), TEXT("Location"));
    Payload->SetNumberField(TEXT("frame"), KeyFrame);
    {
        TSharedPtr<FJsonObject> Value = MakeShared<FJsonObject>();
        Value->SetNumberField(TEXT("x"), 0.0);
        Value->SetNumberField(TEXT("y"), 0.0);
        Value->SetNumberField(TEXT("z"), 300.0);
        Payload->SetObjectField(TEXT("value"), Value);
    }

    FTestResponseCapture Capture;
    TestTrue(TEXT("sequence.add_keyframe handler invoked"),
        InvokeHandlerWithCapture(TEXT("sequence.add_keyframe"), Payload, Capture));
    TestTrue(TEXT("handler reported success"), Capture.bSuccess);

    UMovieScene3DTransformSection* Section = FindTransformSection(MovieScene, BindingGuid);
    if (!TestNotNull(TEXT("transform section created"), Section))
    {
        CleanupTestAsset(FullPath);
        return true;
    }

    const FFrameNumber KeyTick = DisplayFrameToTick(MovieScene, KeyFrame);
    // Same regression assertion for the per-axis branch (#3 in the ticket).
    TestTrue(TEXT("per-axis Location section range contains the authored key tick"),
        Section->GetRange().Contains(KeyTick));

    CleanupTestAsset(FullPath);
    return true;
}
