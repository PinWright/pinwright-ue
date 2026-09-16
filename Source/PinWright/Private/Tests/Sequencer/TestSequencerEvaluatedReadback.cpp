// Copyright (c) 2026 Alexander Penkin. MIT License.

// Red test for F-sequencer-evaluate-readback.
//
// PinWright's sequencer surface can AUTHOR keyframes (sequence.add_keyframe,
// sequencer.add_transform_track, ...) but exposes NO evaluated-state readback:
// there is no way to ask "where is a bound actor at frame N" after keying a track
// (grep Evaluate|EvaluateAt in Handlers/Sequencer/ = zero; the proposed
// sequencer.evaluate_at / get_binding_transform / get_evaluated_property RPCs do
// not exist anywhere in the plugin). The cinematics edit-verify loop therefore has
// no programmatic verification path — the only way to confirm an authored curve is
// a screenshot.
//
// This test authors a two-key Location ramp (X=0 @ frame 0, X=100 @ frame 100) on
// a bound possessable via the REAL registered sequence.add_keyframe handler, then
// asks the proposed sequencer.get_binding_transform RPC for the bound transform at
// the midpoint (frame 50) and asserts the returned X is the interpolated ~50 within
// tolerance — the ticket's acceptance criterion. It routes both authoring and
// readback through the production registration list (InvokeHandlerWithCapture), not
// a re-implemented local evaluator.
//
// Differential property: pre-fix the readback handler is not registered, so the
// registration + readback assertions FAIL, reproducing the capability gap. Once an
// implementer adds a handler that force-evaluates the sequence and returns the
// interpolated transform, the assertions flip green.
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
#include "Evaluation/Blending/MovieSceneBlendType.h"
#include "GameFramework/Actor.h"

namespace
{
    // Distinctly named (avoids anonymous-namespace ODR collision with sibling
    // sequencer test .cpp files when Unity merges TUs): create a real /Game
    // LevelSequence via the registered sequencer.create handler so the readback
    // handler's asset load can resolve it. Empty path + nullptr on failure.
    ULevelSequence* CreateEvalReadbackSequence(FAutomationTestBase& Test, FString& OutFullPath)
    {
        const FString SeqName = FString::Printf(TEXT("MCP_EvalReadbackSeq_%s"),
            *FGuid::NewGuid().ToString(EGuidFormats::Digits));
        const FString DestFolder = TEXT("/Game/MCP_EvalReadbackProbe");
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
                     "host); the registration assertion above still stands"));
            OutFullPath.Reset();
            return nullptr;
        }
        return Cast<ULevelSequence>(UEditorAssetLibrary::LoadAsset(OutFullPath));
    }

    // Author one Location keyframe (X only varies) at the given display frame via
    // the real registered sequence.add_keyframe handler.
    void AuthorEvalReadbackLocationKey(const FString& FullPath, const FString& BindingId,
        double Frame, double X)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("path"), FullPath);
        Payload->SetStringField(TEXT("bindingId"), BindingId);
        Payload->SetStringField(TEXT("property"), TEXT("Location"));
        Payload->SetNumberField(TEXT("frame"), Frame);
        TSharedPtr<FJsonObject> Value = MakeShared<FJsonObject>();
        Value->SetNumberField(TEXT("x"), X);
        Value->SetNumberField(TEXT("y"), 0.0);
        Value->SetNumberField(TEXT("z"), 0.0);
        Payload->SetObjectField(TEXT("value"), Value);
        FTestResponseCapture Capture;
        InvokeHandlerWithCapture(TEXT("sequence.add_keyframe"), Payload, Capture);
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSequencerBindingTransformAtFrameTest,
    "PinWright.Sequencer.EvaluatedReadback.BindingTransformAtFrame",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSequencerBindingTransformAtFrameTest::RunTest(const FString& Parameters)
{
    // Core capability-gap reproduction: an evaluated-transform readback RPC must
    // exist. Pre-fix nothing registers sequencer.get_binding_transform, so this
    // assertion fails — the exact defect the ticket reports.
    TestTrue(TEXT("sequencer.get_binding_transform handler registered"),
        IsHandlerRegistered(TEXT("sequencer.get_binding_transform")));

    // Author a two-key Location ramp (X: 0 -> 100 over frames 0 -> 100) on a bound
    // possessable so a correct readback has something to interpolate.
    FString FullPath;
    ULevelSequence* Sequence = CreateEvalReadbackSequence(*this, FullPath);
    if (!Sequence)
    {
        // The registration failure above already marks this test Fail; without a
        // sequence factory there is nothing further to author or read back.
        return true;
    }
    UMovieScene* MovieScene = Sequence->GetMovieScene();
    if (!TestNotNull(TEXT("MovieScene present"), MovieScene))
    {
        CleanupTestAsset(FullPath);
        return true;
    }

    const FGuid BindingGuid = MovieScene->AddPossessable(TEXT("ProbeActor"), AActor::StaticClass());
    const FString BindingId = BindingGuid.ToString(EGuidFormats::Digits);
    if (!TestTrue(TEXT("binding GUID is valid"), BindingGuid.IsValid()))
    {
        CleanupTestAsset(FullPath);
        return true;
    }

    AuthorEvalReadbackLocationKey(FullPath, BindingId, 0.0, 0.0);
    AuthorEvalReadbackLocationKey(FullPath, BindingId, 100.0, 100.0);

    // Ask the proposed readback RPC for the bound transform at the midpoint frame.
    TSharedPtr<FJsonObject> ReadPayload = MakeShared<FJsonObject>();
    ReadPayload->SetStringField(TEXT("path"), FullPath);
    ReadPayload->SetStringField(TEXT("bindingId"), BindingId);
    ReadPayload->SetNumberField(TEXT("frame"), 50.0);

    FTestResponseCapture ReadCapture;
    const bool bFound = InvokeHandlerWithCapture(
        TEXT("sequencer.get_binding_transform"), ReadPayload, ReadCapture);
    TestTrue(TEXT("sequencer.get_binding_transform invoked (handler present)"), bFound);
    TestTrue(TEXT("readback responded"), ReadCapture.bWasCalled);
    TestTrue(TEXT("readback reported success (no fake failure)"), ReadCapture.bSuccess);

    // Acceptance: the returned location X is the interpolated midpoint (~50) within
    // tolerance. Only reachable once the handler exists and succeeds.
    if (ReadCapture.bSuccess && ReadCapture.Result.IsValid())
    {
        const TSharedPtr<FJsonObject>* Loc = nullptr;
        if (TestTrue(TEXT("readback result carries a location object"),
                ReadCapture.Result->TryGetObjectField(TEXT("location"), Loc) && Loc && Loc->IsValid()))
        {
            double EvaluatedX = 0.0;
            TestTrue(TEXT("location carries an x component"),
                (*Loc)->TryGetNumberField(TEXT("x"), EvaluatedX));
            TestTrue(TEXT("evaluated X at frame 50 is the interpolated midpoint (~50)"),
                FMath::IsNearlyEqual(EvaluatedX, 50.0, 1.0));
        }
    }

    CleanupTestAsset(FullPath);
    return true;
}

// Differential coverage for the composited-readback fix (reviewer concern on
// SequenceHandler.cpp get_binding_transform): the readback must run the MovieScene
// interrogation pipeline, applying cross-section blending — NOT read a single
// section's raw curve. This authors two overlapping transform sections on one
// binding: an ABSOLUTE section pinning Location.X to 100 and an ADDITIVE section
// adding 30. Sequencer composites them to 130 at every frame. The pre-fix
// raw-channel readback evaluated only one section's curve (100 or 30) and could
// never return 130, so this test FAILS against the old implementation and passes
// only once evaluation goes through FSystemInterrogator (the same divergence that
// appears at eased section boundaries, which the interrogation weight also fixes).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSequencerBindingTransformOverlappingBlendTest,
    "PinWright.Sequencer.EvaluatedReadback.OverlappingSectionsBlend",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSequencerBindingTransformOverlappingBlendTest::RunTest(const FString& Parameters)
{
    // Composited readback is only meaningful once the handler exists.
    if (!TestTrue(TEXT("sequencer.get_binding_transform handler registered"),
            IsHandlerRegistered(TEXT("sequencer.get_binding_transform"))))
    {
        return false;
    }

    FString FullPath;
    ULevelSequence* Sequence = CreateEvalReadbackSequence(*this, FullPath);
    if (!Sequence)
    {
        // Helper already warned; without a sequence factory there is nothing to author.
        return true;
    }
    UMovieScene* MovieScene = Sequence->GetMovieScene();
    if (!TestNotNull(TEXT("MovieScene present"), MovieScene))
    {
        CleanupTestAsset(FullPath);
        return true;
    }

    const FGuid BindingGuid = MovieScene->AddPossessable(TEXT("BlendProbeActor"), AActor::StaticClass());
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

    // Wide tick range so both sections cover the eval frame regardless of tick/display rate.
    const TRange<FFrameNumber> WideRange(FFrameNumber(0), FFrameNumber(10000000));

    auto MakeConstantSection =
        [&](EMovieSceneBlendType Blend, double LocationX) -> UMovieScene3DTransformSection*
    {
        UMovieScene3DTransformSection* Section =
            Cast<UMovieScene3DTransformSection>(Track->CreateNewSection());
        if (!Section)
        {
            return nullptr;
        }
        Section->SetRange(WideRange);
        Section->SetBlendType(Blend);
        Track->AddSection(*Section);

        // A single constant key on Location.X (channel 0) -> flat curve = LocationX everywhere.
        TArrayView<FMovieSceneDoubleChannel*> Channels =
            Section->GetChannelProxy().GetChannels<FMovieSceneDoubleChannel>();
        if (Channels.Num() > 0 && Channels[0])
        {
            Channels[0]->GetData().UpdateOrAddKey(FFrameNumber(0), FMovieSceneDoubleValue(LocationX));
        }
        return Section;
    };

    UMovieScene3DTransformSection* AbsSection =
        MakeConstantSection(EMovieSceneBlendType::Absolute, 100.0);
    UMovieScene3DTransformSection* AddSection =
        MakeConstantSection(EMovieSceneBlendType::Additive, 30.0);
    if (!TestNotNull(TEXT("absolute section created"), AbsSection) ||
        !TestNotNull(TEXT("additive section created"), AddSection))
    {
        CleanupTestAsset(FullPath);
        return true;
    }

    TSharedPtr<FJsonObject> ReadPayload = MakeShared<FJsonObject>();
    ReadPayload->SetStringField(TEXT("path"), FullPath);
    ReadPayload->SetStringField(TEXT("bindingId"), BindingId);
    ReadPayload->SetNumberField(TEXT("frame"), 50.0);

    FTestResponseCapture ReadCapture;
    const bool bFound = InvokeHandlerWithCapture(
        TEXT("sequencer.get_binding_transform"), ReadPayload, ReadCapture);
    TestTrue(TEXT("readback invoked (handler present)"), bFound);
    TestTrue(TEXT("readback responded"), ReadCapture.bWasCalled);
    TestTrue(TEXT("readback reported success (no fake failure)"), ReadCapture.bSuccess);

    if (ReadCapture.bSuccess && ReadCapture.Result.IsValid())
    {
        const TSharedPtr<FJsonObject>* Loc = nullptr;
        if (TestTrue(TEXT("readback result carries a location object"),
                ReadCapture.Result->TryGetObjectField(TEXT("location"), Loc) && Loc && Loc->IsValid()))
        {
            double EvaluatedX = 0.0;
            TestTrue(TEXT("location carries an x component"),
                (*Loc)->TryGetNumberField(TEXT("x"), EvaluatedX));
            AddInfo(FString::Printf(
                TEXT("composited Location.X = %f (absolute 100 + additive 30 -> expect ~130)"), EvaluatedX));
            // Differential proof: the composited value blends BOTH sections (~130), which is
            // strictly greater than either section's own curve value. A single-section raw
            // read (the pre-fix behavior) returns <= 100 and fails this assertion.
            TestTrue(
                TEXT("composited Location.X reflects additive blend across overlapping sections "
                     "(~130, above the absolute section's own 100)"),
                EvaluatedX > 100.5 && FMath::IsNearlyEqual(EvaluatedX, 130.0, 2.0));
        }
    }

    CleanupTestAsset(FullPath);
    return true;
}
