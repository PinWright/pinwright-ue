// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for E-sequencer-property-unit-drift (reworded to center the real,
// source-confirmed defect).
//
// sequencer.set_properties advertises playbackStart/playbackEnd as display-rate frame
// numbers (RPC_PARAM_OPT "...(display-rate frame number)"), but before the fix it cast the
// value straight into a tick-resolution FFrameNumber with NO display->tick conversion
// (StartFrame = FFrameNumber(static_cast<int32>(PlaybackStartValue))). So at 24 fps over a
// 24000-tick resolution a caller passing playbackEnd:120 (frame 120 = 5 s) silently got a
// 120-tick (0.005 s) range -- a ~1000x error returned with applied:true and no warning. The
// fix routes both bounds (and the lengthInFrames delta) through
// SequenceHelpers::DisplayFrameToTick, the same helper the keyframe path uses, so the stored
// range honors the documented "frame" contract.
//
// This test drives the REAL registered sequencer.set_properties handler through
// InvokeHandlerWithCapture (production code, not a copy). The fixture is a real /Game
// LevelSequence created in-code via the registered sequencer.create handler -- required
// because sequencer.set_properties resolves its path through UEditorAssetLibrary::LoadAsset,
// which rejects /Engine/Transient paths. The sequence is pinned to an explicit 24 fps
// display rate and 24000 tick resolution, then the test asserts the MovieScene's stored
// playback range is the frame->tick conversion (frame 30 -> 30000 ticks, frame 120 ->
// 120000 ticks), NOT the raw unconverted frame numbers. Counterfactual: revert the
// DisplayFrameToTick calls to the bare FFrameNumber cast and the stored bounds become
// 30 / 120, so every conversion assertion fails. A missing fixture is a hard failure
// (TestTrue/TestNotNull record the error), never a skip.
#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "EditorAssetLibrary.h"
#include "Tests/TestUtils.h"

#include "LevelSequence.h"
#include "MovieScene.h"
#include "Misc/FrameRate.h"
#include "Misc/Guid.h"

namespace
{
    // The tick frame the handler must store for a display-rate Frame, mirroring
    // SequenceHelpers::DisplayFrameToTick (TransformTime from the scene's display rate to its
    // tick resolution, FloorToFrame). Kept local because the production helper is a
    // file-internal static in SequenceHandler.cpp.
    FFrameNumber SetPropsExpectedTick(UMovieScene* MovieScene, double Frame)
    {
        return FFrameRate::TransformTime(
                   FFrameTime(FFrameNumber(static_cast<int32>(Frame))),
                   MovieScene->GetDisplayRate(), MovieScene->GetTickResolution())
            .FloorToFrame();
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSetPropertiesPlaybackRangeFramesToTicksTest,
    "PinWright.Sequencer.SetProperties.PlaybackRangeFramesToTicks",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSetPropertiesPlaybackRangeFramesToTicksTest::RunTest(const FString& Parameters)
{
    TestTrue(TEXT("sequencer.set_properties handler registered"),
        IsHandlerRegistered(TEXT("sequencer.set_properties")));

    // Build the fixture in-code via the registered sequencer.create handler: a real /Game
    // asset so sequencer.set_properties' UEditorAssetLibrary::LoadAsset can resolve it.
    const FString SeqName = FString::Printf(TEXT("MCP_SetPropsUnits_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    const FString DestFolder = TEXT("/Game/MCP_SetPropsUnitsProbe");
    const FString FullPath = FString::Printf(TEXT("%s/%s"), *DestFolder, *SeqName);

    TSharedPtr<FJsonObject> CreatePayload = MakeShared<FJsonObject>();
    CreatePayload->SetStringField(TEXT("name"), SeqName);
    CreatePayload->SetStringField(TEXT("path"), DestFolder);
    FTestResponseCapture CreateCapture;
    TestTrue(TEXT("sequencer.create handler registered"),
        InvokeHandlerWithCapture(TEXT("sequencer.create"), CreatePayload, CreateCapture));
    if (!TestTrue(TEXT("sequencer.create reported success"), CreateCapture.bSuccess))
    {
        CleanupTestAsset(FullPath);
        return true; // hard failure already recorded
    }
    if (!TestTrue(TEXT("probe sequence asset exists after create"),
            UEditorAssetLibrary::DoesAssetExist(FullPath)))
    {
        CleanupTestAsset(FullPath);
        return true;
    }

    ULevelSequence* Sequence = Cast<ULevelSequence>(UEditorAssetLibrary::LoadAsset(FullPath));
    if (!TestNotNull(TEXT("probe LevelSequence loads"), Sequence))
    {
        CleanupTestAsset(FullPath);
        return true;
    }
    UMovieScene* MovieScene = Sequence->GetMovieScene();
    if (!TestNotNull(TEXT("MovieScene present"), MovieScene))
    {
        CleanupTestAsset(FullPath);
        return true;
    }

    // Pin a display rate distinct from the tick resolution so frame != tick and the
    // conversion is discriminating: 24 fps display over 24000-tick resolution = 1000 ticks
    // per frame.
    MovieScene->SetTickResolutionDirectly(FFrameRate(24000, 1));
    MovieScene->SetDisplayRate(FFrameRate(24, 1));

    const double StartFrameInput = 30.0;   // frame 30  -> 30000 ticks (1.25 s)
    const double EndFrameInput = 120.0;    // frame 120 -> 120000 ticks (5 s)

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("path"), FullPath);
    Payload->SetNumberField(TEXT("playbackStart"), StartFrameInput);
    Payload->SetNumberField(TEXT("playbackEnd"), EndFrameInput);

    FTestResponseCapture Capture;
    TestTrue(TEXT("sequencer.set_properties handler invoked"),
        InvokeHandlerWithCapture(TEXT("sequencer.set_properties"), Payload, Capture));
    TestTrue(TEXT("handler responded"), Capture.bWasCalled);
    if (!TestTrue(TEXT("handler reported success"), Capture.bSuccess))
    {
        CleanupTestAsset(FullPath);
        return true;
    }

    // Ground truth: the stored playback range must be the frame->tick conversion, not the
    // raw display-frame numbers.
    const FFrameNumber ExpectedStart = SetPropsExpectedTick(MovieScene, StartFrameInput); // 30000
    const FFrameNumber ExpectedEnd = SetPropsExpectedTick(MovieScene, EndFrameInput);     // 120000
    const TRange<FFrameNumber> Stored = MovieScene->GetPlaybackRange();

    TestEqual(TEXT("stored playback start is the frame->tick conversion (30 -> 30000)"),
        Stored.GetLowerBoundValue().Value, ExpectedStart.Value);
    TestEqual(TEXT("stored playback end is the frame->tick conversion (120 -> 120000)"),
        Stored.GetUpperBoundValue().Value, ExpectedEnd.Value);

    // Anti-regression: the bounds must NOT be the raw, unconverted frame numbers the pre-fix
    // bare cast stored (30 and 120 ticks = a ~1000x-too-short range).
    TestNotEqual(TEXT("stored start is not the unconverted raw frame (30)"),
        Stored.GetLowerBoundValue().Value, static_cast<int32>(StartFrameInput));
    TestNotEqual(TEXT("stored end is not the unconverted raw frame (120)"),
        Stored.GetUpperBoundValue().Value, static_cast<int32>(EndFrameInput));

    // The response echoes the tick-resolution range (consistent with get_properties) and
    // carries tickResolution so the echoed ticks are self-describing.
    if (TestTrue(TEXT("response result present"), Capture.Result.IsValid()))
    {
        double EchoedEnd = 0.0;
        TestTrue(TEXT("response echoes playbackEnd"),
            Capture.Result->TryGetNumberField(TEXT("playbackEnd"), EchoedEnd));
        TestEqual(TEXT("echoed playbackEnd matches the stored tick range"),
            static_cast<int32>(EchoedEnd), ExpectedEnd.Value);
        const TSharedPtr<FJsonObject>* TickResolutionObj = nullptr;
        TestTrue(TEXT("response carries tickResolution"),
            Capture.Result->TryGetObjectField(TEXT("tickResolution"), TickResolutionObj));
    }

    CleanupTestAsset(FullPath);
    return true;
}
