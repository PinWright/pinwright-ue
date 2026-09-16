// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for B-sequencer-get-binding-transform-rotation-fields-rotated.
//
// sequencer.get_binding_transform reported the rotation triple one position out: the
// number returned under "roll" was the authored pitch, "pitch" carried the yaw and
// "yaw" carried the roll. The permutation comes from the engine, not from the emit
// side — FInterrogationChannels::QueryLocalSpaceTransforms' fully-animated fast path
// builds its result as FIntermediate3DTransform(LocX, LocY, LocZ, RotY, RotZ, RotX, ...)
// (MovieSceneInterrogationLinker.cpp, identical in 5.3-5.8), so Rotation.Y lands in
// R_X (Roll), Rotation.Z in R_Y (Pitch) and Rotation.X in R_Z (Yaw). The fix reads the
// same interrogation back through the ComponentTransform property composites, which map
// DoubleResult[3..5] -> R_X/R_Y/R_Z as declared.
//
// The fixture keys all three rotation channels with THREE DISTINCT non-zero values and
// evaluates at a NON-KEY frame, so the assertion is only satisfiable by a correctly
// labelled readback: a camera-shaped rotation (roll 0, yaw 0) passes both before and
// after the fix and proves nothing.
//
// Differential property: pre-fix the response reads roll=-22 / pitch=130 / yaw=5 for an
// authored roll=5 / pitch=-22 / yaw=130, so every one of the three assertions below
// fails. Post-fix each component appears under its own key.
#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "EditorAssetLibrary.h"
#include "Tests/TestSkipReporting.h"
#include "Tests/TestUtils.h"

#include "LevelSequence.h"
#include "MovieScene.h"
#include "Tracks/MovieScene3DTransformTrack.h"
#include "Sections/MovieScene3DTransformSection.h"
#include "Channels/MovieSceneDoubleChannel.h"
#include "Channels/MovieSceneChannelProxy.h"
#include "GameFramework/Actor.h"

namespace
{
    // Authored rotation fixture: three distinct, non-zero, non-equal components so any
    // permutation of the emitted triple is detectable. Keys run 0 -> 2x these values over
    // frames 0 -> 100 with linear interpolation, so the midpoint frame evaluates to exactly
    // these numbers without landing on a key.
    constexpr double RotationProbeRoll = 5.0;
    constexpr double RotationProbePitch = -22.0;
    constexpr double RotationProbeYaw = 130.0;

    constexpr double RotationProbeStartFrame = 0.0;
    constexpr double RotationProbeEndFrame = 100.0;
    constexpr double RotationProbeEvalFrame = 50.0;

    // Distinctly named (anonymous-namespace ODR collisions with sibling sequencer test .cpp
    // files when Unity merges TUs): create a real /Game LevelSequence through the registered
    // sequencer.create handler so the readback handler's asset load can resolve it.
    ULevelSequence* CreateRotationFieldProbeSequence(FAutomationTestBase& Test, FString& OutFullPath)
    {
        const FString SeqName = FString::Printf(TEXT("MCP_RotationFieldSeq_%s"),
            *FGuid::NewGuid().ToString(EGuidFormats::Digits));
        const FString DestFolder = TEXT("/Game/MCP_RotationFieldProbe");
        OutFullPath = FString::Printf(TEXT("%s/%s"), *DestFolder, *SeqName);

        TSharedPtr<FJsonObject> CreatePayload = MakeShared<FJsonObject>();
        CreatePayload->SetStringField(TEXT("name"), SeqName);
        CreatePayload->SetStringField(TEXT("path"), DestFolder);
        FTestResponseCapture CreateCapture;
        InvokeHandlerWithCapture(TEXT("sequencer.create"), CreatePayload, CreateCapture);

        if (!CreateCapture.bWasCalled || !CreateCapture.bSuccess ||
            !UEditorAssetLibrary::DoesAssetExist(OutFullPath))
        {
            // No sequence factory in this host, so the rotation-field assertions below cannot
            // run. Routed through the one emitter rather than spelling the wire literal here.
            PinWrightTestSkip::SkipAssertions(Test, TEXT("sequence-factory-unavailable"),
                TEXT("could not create a probe sequence "
                     "(sequence factory unavailable in this host)"));
            OutFullPath.Reset();
            return nullptr;
        }
        return Cast<ULevelSequence>(UEditorAssetLibrary::LoadAsset(OutFullPath));
    }

    // Display-rate frame -> tick-resolution frame, mirroring SequenceHelpers::DisplayFrameToTick
    // so the keys land where the handler's own "frame" parameter looks for them.
    FFrameNumber RotationProbeDisplayFrameToTick(UMovieScene* MovieScene, double DisplayFrame)
    {
        return FFrameRate::TransformTime(FFrameTime(FFrameNumber(static_cast<int32>(DisplayFrame))),
                   MovieScene->GetDisplayRate(), MovieScene->GetTickResolution())
            .FloorToFrame();
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSequencerBindingTransformRotationFieldsTest,
    "PinWright.Sequencer.EvaluatedReadback.RotationFieldsMatchAuthoredComponents",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSequencerBindingTransformRotationFieldsTest::RunTest(const FString& Parameters)
{
    if (!TestTrue(TEXT("sequencer.get_binding_transform handler registered"),
            IsHandlerRegistered(TEXT("sequencer.get_binding_transform"))))
    {
        return false;
    }

    FString FullPath;
    ULevelSequence* Sequence = CreateRotationFieldProbeSequence(*this, FullPath);
    if (!Sequence)
    {
        // Helper already emitted the skip marker.
        return true;
    }
    UMovieScene* MovieScene = Sequence->GetMovieScene();
    if (!TestNotNull(TEXT("MovieScene present"), MovieScene))
    {
        CleanupTestAsset(FullPath);
        return true;
    }

    const FGuid BindingGuid =
        MovieScene->AddPossessable(TEXT("RotationProbeActor"), AActor::StaticClass());
    const FString BindingId = BindingGuid.ToString(EGuidFormats::Digits);
    if (!TestTrue(TEXT("binding GUID is valid"), BindingGuid.IsValid()))
    {
        CleanupTestAsset(FullPath);
        return true;
    }

    UMovieScene3DTransformTrack* Track = MovieScene->AddTrack<UMovieScene3DTransformTrack>(BindingGuid);
    if (!TestNotNull(TEXT("transform track created"), Track))
    {
        CleanupTestAsset(FullPath);
        return true;
    }

    UMovieScene3DTransformSection* Section =
        Cast<UMovieScene3DTransformSection>(Track->CreateNewSection());
    if (!TestNotNull(TEXT("transform section created"), Section))
    {
        CleanupTestAsset(FullPath);
        return true;
    }

    const FFrameNumber StartTick = RotationProbeDisplayFrameToTick(MovieScene, RotationProbeStartFrame);
    const FFrameNumber EndTick = RotationProbeDisplayFrameToTick(MovieScene, RotationProbeEndFrame);
    // Upper bound is exclusive, so extend past the last key to keep it inside the section.
    Section->SetRange(TRange<FFrameNumber>(StartTick, EndTick + FFrameNumber(1)));
    Track->AddSection(*Section);

    // Channel layout on a transform section is 0-2 Location, 3-5 Rotation, 6-8 Scale, 9 Weight;
    // the engine labels Rotation.X "Roll", Rotation.Y "Pitch", Rotation.Z "Yaw"
    // (MovieScene3DTransformSection.cpp F3DTransformChannelEditorData).
    TArrayView<FMovieSceneDoubleChannel*> Channels =
        Section->GetChannelProxy().GetChannels<FMovieSceneDoubleChannel>();
    if (!TestTrue(TEXT("section exposes the rotation channels (indices 3-5)"), Channels.Num() >= 6))
    {
        CleanupTestAsset(FullPath);
        return true;
    }

    // Ramp each rotation channel 0 -> 2x its target so the midpoint frame is the target and
    // is NOT a key: the value has to come out of the interpolation pipeline.
    auto KeyLinearRamp = [&](int32 ChannelIndex, double EndValue)
    {
        FMovieSceneDoubleChannel* Channel = Channels[ChannelIndex];
        if (!Channel)
        {
            return;
        }
        FMovieSceneDoubleValue StartValue(0.0);
        StartValue.InterpMode = RCIM_Linear;
        FMovieSceneDoubleValue FinalValue(EndValue);
        FinalValue.InterpMode = RCIM_Linear;
        Channel->GetData().UpdateOrAddKey(StartTick, StartValue);
        Channel->GetData().UpdateOrAddKey(EndTick, FinalValue);
    };
    KeyLinearRamp(3, RotationProbeRoll * 2.0);
    KeyLinearRamp(4, RotationProbePitch * 2.0);
    KeyLinearRamp(5, RotationProbeYaw * 2.0);

    TSharedPtr<FJsonObject> ReadPayload = MakeShared<FJsonObject>();
    ReadPayload->SetStringField(TEXT("path"), FullPath);
    ReadPayload->SetStringField(TEXT("bindingId"), BindingId);
    ReadPayload->SetNumberField(TEXT("frame"), RotationProbeEvalFrame);

    FTestResponseCapture ReadCapture;
    const bool bFound = InvokeHandlerWithCapture(
        TEXT("sequencer.get_binding_transform"), ReadPayload, ReadCapture);
    TestTrue(TEXT("readback invoked (handler present)"), bFound);
    TestTrue(TEXT("readback responded"), ReadCapture.bWasCalled);
    TestTrue(TEXT("readback reported success (no fake failure)"), ReadCapture.bSuccess);

    if (ReadCapture.bSuccess && ReadCapture.Result.IsValid())
    {
        const TSharedPtr<FJsonObject>* Rot = nullptr;
        if (TestTrue(TEXT("readback result carries a rotation object"),
                ReadCapture.Result->TryGetObjectField(TEXT("rotation"), Rot) && Rot && Rot->IsValid()))
        {
            double ReportedRoll = 0.0;
            double ReportedPitch = 0.0;
            double ReportedYaw = 0.0;
            TestTrue(TEXT("rotation carries a roll component"),
                (*Rot)->TryGetNumberField(TEXT("roll"), ReportedRoll));
            TestTrue(TEXT("rotation carries a pitch component"),
                (*Rot)->TryGetNumberField(TEXT("pitch"), ReportedPitch));
            TestTrue(TEXT("rotation carries a yaw component"),
                (*Rot)->TryGetNumberField(TEXT("yaw"), ReportedYaw));

            AddInfo(FString::Printf(
                TEXT("authored roll=%g pitch=%g yaw=%g at frame %g; reported roll=%g pitch=%g yaw=%g"),
                RotationProbeRoll, RotationProbePitch, RotationProbeYaw, RotationProbeEvalFrame,
                ReportedRoll, ReportedPitch, ReportedYaw));

            // Each component under its own key. Pre-fix the triple is rotated one slot left
            // (roll reads the pitch, pitch reads the yaw, yaw reads the roll), so all three fail.
            TestTrue(FString::Printf(TEXT("reported roll is the authored roll (%g, not the pitch)"),
                         RotationProbeRoll),
                FMath::IsNearlyEqual(ReportedRoll, RotationProbeRoll, 0.5));
            TestTrue(FString::Printf(TEXT("reported pitch is the authored pitch (%g, not the yaw)"),
                         RotationProbePitch),
                FMath::IsNearlyEqual(ReportedPitch, RotationProbePitch, 0.5));
            TestTrue(FString::Printf(TEXT("reported yaw is the authored yaw (%g, not the roll)"),
                         RotationProbeYaw),
                FMath::IsNearlyEqual(ReportedYaw, RotationProbeYaw, 0.5));
        }
    }

    CleanupTestAsset(FullPath);
    return true;
}
