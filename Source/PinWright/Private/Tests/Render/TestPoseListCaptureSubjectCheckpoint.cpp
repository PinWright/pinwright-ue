// Copyright (c) 2026 Alexander Penkin. MIT License.

// The pose-0 repeatability control of a TIME-DRIVEN pose set, through the subject state checkpoint
// seam (board F-pose-repeatability-checkpoint).
//
// The stub subject is deliberately NON-IDEMPOTENT, the Niagara shape: its time setter ADVANCES
// from the current state (State += t), so replaying the setter for the control would land somewhere
// else entirely. The only way the control can match pose 0 is a rewind to the pose-0 checkpoint,
// and the only way the subject can end where the set left it is a restore of the end-of-set one.
//
// No viewport, world or GPU: RunPoseListCapture is the production sequence with the draw injected,
// so these run identically under NullRHI and need no skip path.
#include "Misc/AutomationTest.h"
#include "Handlers/Render/PoseListCapture.h"

#include "Dom/JsonObject.h"

namespace TestPoseListSubjectCheckpointHelpers
{
    constexpr int32 Edge = 8;

    struct FStubSubject
    {
        double State = 0.0;
        int32 SetterCalls = 0;
        int32 Checkpoints = 0;
        int32 Restores = 0;
        // Index (0-based, in checkpoint order) of a checkpoint whose restorer refuses.
        int32 FailRestoreOfCheckpoint = INDEX_NONE;

        // Every frame drawn, and the subject state it was drawn at.
        TArray<bool> FrameIsDisposable;
        TArray<double> FrameState;
        bool bFailControlFrame = false;
        int32 RealShots = 0;
        int32 PoseCount = 0;

        PinWrightPoseCapture::FSubjectTimeSetter MakeTimeSetter()
        {
            return [this](double TimeSeconds, FString&, FString&) -> bool
            {
                ++SetterCalls;
                State += TimeSeconds;
                return true;
            };
        }

        PinWrightPoseCapture::FSubjectStateCheckpointer MakeCheckpointer()
        {
            return [this](PinWrightPoseCapture::FSubjectStateRestorer& OutRestore,
                FString&, FString&) -> bool
            {
                const int32 CheckpointIndex = Checkpoints++;
                const double Saved = State;
                const bool bFail = CheckpointIndex == FailRestoreOfCheckpoint;
                OutRestore = [this, Saved, bFail](FString& OutErrCode, FString& OutErrMsg) -> bool
                {
                    ++Restores;
                    if (bFail)
                    {
                        // A half-done restore: the state moves, then the restorer refuses, so only
                        // the end-of-set put-back can bring the subject back where the set left it.
                        State = -100.0;
                        OutErrCode = TEXT("STUB");
                        OutErrMsg = TEXT("stub restorer refused");
                        return false;
                    }
                    State = Saved;
                    return true;
                };
                return true;
            };
        }

        PinWrightPoseCapture::FPoseFrameCapturer MakeCapturer()
        {
            return [this](const PinWrightRenderCapture::FViewportCaptureRequest& Frame,
                bool bWarmupFrame, PinWrightRenderCapture::FViewportCaptureOutput& OutCapture,
                FString& OutErrorCode, FString& OutErrorMessage) -> bool
            {
                // The control is the only disposable frame drawn after every real shot.
                if (bWarmupFrame && RealShots == PoseCount && bFailControlFrame)
                {
                    OutErrorCode = TEXT("STUB");
                    OutErrorMessage = TEXT("stub control frame failed");
                    return false;
                }
                if (!bWarmupFrame)
                {
                    ++RealShots;
                }
                FrameIsDisposable.Add(bWarmupFrame);
                FrameState.Add(State);
                OutCapture.Width = Edge;
                OutCapture.Height = Edge;
                if (Frame.bRetainPixels)
                {
                    // The picture is a pure function of the subject state, so any state difference
                    // between pose 0 and the control is a pixel difference.
                    const uint8 Level = static_cast<uint8>(FMath::Clamp(State * 20.0, 0.0, 255.0));
                    OutCapture.Pixels.Init(FColor(Level, Level, Level, 255), Edge * Edge);
                }
                return true;
            };
        }
    };

    PinWrightPoseCapture::FPoseListCaptureRequest MakeTimeDrivenRequest(FStubSubject& Subject)
    {
        PinWrightPoseCapture::FPoseListCaptureRequest Request;
        Request.Width = Edge;
        Request.Height = Edge;
        Request.bMeasurePoseRepeatability = true;
        Request.Poses.SetNum(3);
        for (int32 Index = 0; Index < Request.Poses.Num(); ++Index)
        {
            Request.Poses[Index].SubjectTimeSeconds = static_cast<float>(Index + 1);
        }
        Request.SubjectTimeSetter = Subject.MakeTimeSetter();
        Subject.PoseCount = Request.Poses.Num();
        return Request;
    }

    TSharedPtr<FJsonObject> CheckpointBlock(const PinWrightPoseCapture::FPoseListCaptureOutput& Output)
    {
        const TSharedPtr<FJsonObject> PoseSet = PinWrightPoseCapture::MakePoseSetInfoObject(Output);
        const TSharedPtr<FJsonObject>* Repeatability = nullptr;
        const TSharedPtr<FJsonObject>* Checkpoint = nullptr;
        if (PoseSet->TryGetObjectField(TEXT("poseRepeatability"), Repeatability)
            && (*Repeatability)->TryGetObjectField(TEXT("subjectCheckpoint"), Checkpoint))
        {
            return *Checkpoint;
        }
        return nullptr;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPoseListSubjectCheckpointRewindsControlTest,
    "PinWright.render.pose_list.subject_checkpoint.RewindsTimeDrivenControl",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPoseListSubjectCheckpointRewindsControlTest::RunTest(const FString& Parameters)
{
    using namespace TestPoseListSubjectCheckpointHelpers;
    FStubSubject Subject;
    PinWrightPoseCapture::FPoseListCaptureRequest Request = MakeTimeDrivenRequest(Subject);
    Request.SubjectStateCheckpointer = Subject.MakeCheckpointer();

    PinWrightPoseCapture::FPoseListCaptureOutput Output;
    FString ErrorCode;
    FString ErrorMessage;
    if (!TestTrue(TEXT("the time-driven set captures"), PinWrightPoseCapture::RunPoseListCapture(
            Request, Subject.MakeCapturer(), Output, ErrorCode, ErrorMessage)))
    {
        return false;
    }
    // Fixture precondition: the setter really is non-idempotent, so a replay could not pass.
    TestEqual(TEXT("the set advanced the subject to 1+2+3 before the control"), Subject.FrameState.Num() > 4
        ? Subject.FrameState[3] : -1.0, 6.0);

    TestEqual(TEXT("the time setter ran once per pose and never for the control"), Subject.SetterCalls, 3);
    TestEqual(TEXT("warm-up, three shots and one control were drawn"), Subject.FrameState.Num(), 5);
    TestEqual(TEXT("two checkpoints: pose 0 and end of set"), Subject.Checkpoints, 2);
    TestEqual(TEXT("two restores: the rewind and the put-back"), Subject.Restores, 2);
    if (Subject.FrameState.Num() == 5)
    {
        TestEqual(TEXT("the control frame was drawn at the pose-0 state"),
            Subject.FrameState[4], Subject.FrameState[1]);
    }
    TestEqual(TEXT("the subject ends where the set left it"), Subject.State, 6.0);

    TestTrue(TEXT("the control was measured"), Output.bPoseRepeatabilityMeasured);
    TestEqual(TEXT("the rewound control matches pose 0 exactly"), Output.PoseRepeatabilityMaxDelta, 0);
    TestTrue(TEXT("the subject was rewound for the control"), Output.bSubjectRewoundForControl);
    TestTrue(TEXT("the end-of-set state was restored"), Output.bSubjectRestoredAfterControl);

    const TSharedPtr<FJsonObject> Checkpoint = CheckpointBlock(Output);
    if (TestTrue(TEXT("poseRepeatability.subjectCheckpoint is published"), Checkpoint.IsValid()))
    {
        TestTrue(TEXT("available"), Checkpoint->GetBoolField(TEXT("available")));
        TestTrue(TEXT("rewoundForControl"), Checkpoint->GetBoolField(TEXT("rewoundForControl")));
        TestTrue(TEXT("restoredAfterControl"), Checkpoint->GetBoolField(TEXT("restoredAfterControl")));
        TestFalse(TEXT("no restore warning"), Checkpoint->HasField(TEXT("restoreWarning")));
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPoseListSubjectCheckpointUnavailableTest,
    "PinWright.render.pose_list.subject_checkpoint.UnavailableKindReportsReason",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPoseListSubjectCheckpointUnavailableTest::RunTest(const FString& Parameters)
{
    using namespace TestPoseListSubjectCheckpointHelpers;
    FStubSubject Subject;
    PinWrightPoseCapture::FPoseListCaptureRequest Request = MakeTimeDrivenRequest(Subject);
    Request.SubjectStateCheckpointUnavailableReason = TEXT("stub kind cannot snapshot");

    PinWrightPoseCapture::FPoseListCaptureOutput Output;
    FString ErrorCode;
    FString ErrorMessage;
    if (!TestTrue(TEXT("the time-driven set captures"), PinWrightPoseCapture::RunPoseListCapture(
            Request, Subject.MakeCapturer(), Output, ErrorCode, ErrorMessage)))
    {
        return false;
    }
    TestEqual(TEXT("no control frame: warm-up and three shots only"), Subject.FrameState.Num(), 4);
    TestEqual(TEXT("the setter was never replayed"), Subject.SetterCalls, 3);
    TestFalse(TEXT("the control was not measured"), Output.bPoseRepeatabilityMeasured);
    TestTrue(TEXT("the reason names the kind's own explanation"),
        Output.PoseRepeatabilityNotMeasuredReason.Contains(TEXT("stub kind cannot snapshot")));

    const TSharedPtr<FJsonObject> Checkpoint = CheckpointBlock(Output);
    if (TestTrue(TEXT("poseRepeatability.subjectCheckpoint is published"), Checkpoint.IsValid()))
    {
        TestFalse(TEXT("available is false"), Checkpoint->GetBoolField(TEXT("available")));
        TestEqual(TEXT("unavailableReason is the provider's"),
            Checkpoint->GetStringField(TEXT("unavailableReason")), FString(TEXT("stub kind cannot snapshot")));
        TestFalse(TEXT("nothing was rewound"), Checkpoint->GetBoolField(TEXT("rewoundForControl")));
        TestFalse(TEXT("no restore was attempted"), Checkpoint->HasField(TEXT("restoredAfterControl")));
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPoseListSubjectCheckpointRestoresOnFailureTest,
    "PinWright.render.pose_list.subject_checkpoint.RestoresEndStateOnEveryExit",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPoseListSubjectCheckpointRestoresOnFailureTest::RunTest(const FString& Parameters)
{
    using namespace TestPoseListSubjectCheckpointHelpers;

    // A failed control frame still puts the end-of-set state back.
    {
        FStubSubject Subject;
        Subject.bFailControlFrame = true;
        PinWrightPoseCapture::FPoseListCaptureRequest Request = MakeTimeDrivenRequest(Subject);
        Request.SubjectStateCheckpointer = Subject.MakeCheckpointer();
        PinWrightPoseCapture::FPoseListCaptureOutput Output;
        FString ErrorCode;
        FString ErrorMessage;
        TestTrue(TEXT("a failed control does not fail the set"), PinWrightPoseCapture::RunPoseListCapture(
            Request, Subject.MakeCapturer(), Output, ErrorCode, ErrorMessage));
        TestFalse(TEXT("the failed control is not measured"), Output.bPoseRepeatabilityMeasured);
        TestTrue(TEXT("the reason is the control's failure"),
            Output.PoseRepeatabilityNotMeasuredReason.Contains(TEXT("stub control frame failed")));
        TestTrue(TEXT("the end-of-set state was restored after the failed control"),
            Output.bSubjectRestoredAfterControl);
        TestEqual(TEXT("the subject ends where the set left it"), Subject.State, 6.0);
    }

    // A failed rewind still puts the end-of-set state back, and no control is drawn.
    {
        FStubSubject Subject;
        Subject.FailRestoreOfCheckpoint = 0;  // the pose-0 checkpoint
        PinWrightPoseCapture::FPoseListCaptureRequest Request = MakeTimeDrivenRequest(Subject);
        Request.SubjectStateCheckpointer = Subject.MakeCheckpointer();
        PinWrightPoseCapture::FPoseListCaptureOutput Output;
        FString ErrorCode;
        FString ErrorMessage;
        TestTrue(TEXT("a failed rewind does not fail the set"), PinWrightPoseCapture::RunPoseListCapture(
            Request, Subject.MakeCapturer(), Output, ErrorCode, ErrorMessage));
        TestEqual(TEXT("both restorers ran: the refused rewind and the put-back"), Subject.Restores, 2);
        TestFalse(TEXT("the subject is not reported as rewound"), Output.bSubjectRewoundForControl);
        TestFalse(TEXT("no control frame was taken"), Output.bPoseRepeatabilityControlShotTaken);
        TestEqual(TEXT("no control frame was drawn: warm-up and three shots only"), Subject.FrameState.Num(), 4);
        TestFalse(TEXT("the failed rewind is not measured"), Output.bPoseRepeatabilityMeasured);
        TestTrue(TEXT("the reason is the rewind's failure"),
            Output.PoseRepeatabilityNotMeasuredReason.Contains(TEXT("stub restorer refused")));
        TestTrue(TEXT("the end-of-set state was restored after the failed rewind"),
            Output.bSubjectRestoredAfterControl);
        TestEqual(TEXT("the subject ends where the set left it"), Subject.State, 6.0);
    }

    // A refused put-back is reported, never hidden.
    {
        FStubSubject Subject;
        Subject.FailRestoreOfCheckpoint = 1;  // the end-of-set checkpoint
        PinWrightPoseCapture::FPoseListCaptureRequest Request = MakeTimeDrivenRequest(Subject);
        Request.SubjectStateCheckpointer = Subject.MakeCheckpointer();
        PinWrightPoseCapture::FPoseListCaptureOutput Output;
        FString ErrorCode;
        FString ErrorMessage;
        TestTrue(TEXT("the set still captures"), PinWrightPoseCapture::RunPoseListCapture(
            Request, Subject.MakeCapturer(), Output, ErrorCode, ErrorMessage));
        TestTrue(TEXT("the control itself was measured"), Output.bPoseRepeatabilityMeasured);
        TestFalse(TEXT("the put-back is reported as failed"), Output.bSubjectRestoredAfterControl);
        const TSharedPtr<FJsonObject> Checkpoint = CheckpointBlock(Output);
        if (TestTrue(TEXT("poseRepeatability.subjectCheckpoint is published"), Checkpoint.IsValid()))
        {
            TestFalse(TEXT("restoredAfterControl is false"),
                Checkpoint->GetBoolField(TEXT("restoredAfterControl")));
            FString Warning;
            TestTrue(TEXT("the restore warning carries the restorer's reason"),
                Checkpoint->TryGetStringField(TEXT("restoreWarning"), Warning)
                && Warning.Contains(TEXT("stub restorer refused")));
        }
    }
    return true;
}
