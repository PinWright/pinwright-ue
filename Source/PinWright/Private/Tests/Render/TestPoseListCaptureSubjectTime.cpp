// Copyright (c) 2026 Alexander Penkin. MIT License.

// Tests for what the pose-list capture primitive does AROUND each frame: driving the subject to
// the instant a pose asks for, deciding whether the subject was in the frame at all, and reporting
// the set.
//
// WHY THESE NEED NO VIEWPORT, NO WORLD AND NO GPU. PinWrightPoseCapture::RunPoseListCapture is the
// production sequence with the one frame-drawing step injected as a closure -- CaptureCameraPoses
// is that function bound to the real viewport capture and nothing else. So every assertion below
// runs against the shipping code path with a stub in place of the draw, and none of them can take
// a conditional-skip path and report success without having asserted anything. That is not a
// stylistic preference here: board ticket B-test-skips-assertions-silently fired on this exact
// cluster of verbs, where PinnedCapturesAreIdentical skipped its only substantive assertions in
// 3 of 3 runs because another process held the GPU, and the suite counted it green.
//
// WHAT IS DEFENDED, in order of how much it costs when it breaks:
//
//  * THE SUBJECT TIME IS APPLIED ONCE PER POSE. The warm-up frame reuses Poses[0], so a driver
//    that re-applied per frame would advance a Niagara simulation twice for the first shot and
//    once for every other -- one frame in the set silently older than it claims. The count is
//    asserted against Poses.Num(), with the warm-up's own frame asserted separately so that "3"
//    cannot pass for the opposite reason (the warm-up never having run at all).
//  * A POSE TIME WITH NO SETTER IS REFUSED. Dropping it returns a valid PNG of the wrong moment,
//    which is indistinguishable from a PNG of the right one.
//  * FRAMING IS MEASURED AGAINST THE POSE THE RENDERER RESOLVED TO. An orbit-mode preview viewport
//    derives the eye from its own pivot and keeps only the requested location's distance, so a
//    capture can be perfectly valid and aimed at nothing. Framing measured off the REQUEST would
//    call that framed, which is exactly the bug the framing check exists to catch.
//  * THE WARM-UP FILE WARNING SAYS WHICH WARM-UP FACT IT MEANS. `warmupWarning` is already taken,
//    inside `viewport.warmup`, for "the frame never settled".
#include "Misc/AutomationTest.h"
#include "Handlers/Render/PoseListCapture.h"

#include "Handlers/ErrorCodes.h"

#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace
{
    // Prefixed for the same reason as every other helper in Tests/Render: anonymous namespaces in
    // one Unity translation unit merge, so a bare name a sibling file also uses is a latent ODR
    // clash rather than a compile error.
    struct FPoseListTestDrawnFrame
    {
        PinWrightRenderCapture::FViewportCaptureRequest Frame;
        bool bWarmupFrame = false;
        // How many subject-time applications had happened when this frame was drawn. The ORDERING
        // assertion: a warm-up frame drawn at 0 is a frame of the wrong instant.
        int32 SubjectTimeApplicationsBefore = 0;
    };

    // The injected frame-drawing step, recording what it was asked to draw and in what order.
    struct FPoseListTestStubViewport
    {
        TArray<FPoseListTestDrawnFrame> Frames;
        // Successful applications, in the order they were applied.
        TArray<double> SubjectTimes;
        // Every entry into the setter, successes and refusals alike.
        int32 SetterCalls = 0;
        int32 ShotsDrawn = 0;

        // What the stubbed renderer resolves the camera to. Unset echoes the request, which is
        // what a viewport that honoured the aim does; setting one is a viewport that did not.
        TOptional<FVector> EffectiveLocationOverride;
        TOptional<FRotator> EffectiveRotationOverride;

        // Path reported for the throwaway frame. Empty means "the capture produced no file", which
        // is also what keeps a test off the filesystem entirely.
        FString WarmupPath;

        int32 FailSetterAtCall = INDEX_NONE;
        int32 FailAtShotIndex = INDEX_NONE;

        PinWrightPoseCapture::FSubjectTimeSetter MakeTimeSetter()
        {
            return [this](double TimeSeconds, FString& OutErrCode, FString& OutErrMsg) -> bool
            {
                const int32 CallIndex = SetterCalls++;
                if (CallIndex == FailSetterAtCall)
                {
                    OutErrCode = ErrorCodes::ERR_CAPTURE_FAILED;
                    OutErrMsg = TEXT("stub subject time setter refused");
                    return false;
                }
                SubjectTimes.Add(TimeSeconds);
                return true;
            };
        }

        PinWrightPoseCapture::FPoseFrameCapturer MakeCapturer()
        {
            return [this](const PinWrightRenderCapture::FViewportCaptureRequest& Frame,
                bool bWarmupFrame,
                PinWrightRenderCapture::FViewportCaptureOutput& OutCapture,
                FString& OutErrorCode,
                FString& OutErrorMessage) -> bool
            {
                FPoseListTestDrawnFrame Drawn;
                Drawn.Frame = Frame;
                Drawn.bWarmupFrame = bWarmupFrame;
                Drawn.SubjectTimeApplicationsBefore = SubjectTimes.Num();
                Frames.Add(Drawn);

                if (!bWarmupFrame)
                {
                    const int32 ShotIndex = ShotsDrawn++;
                    if (ShotIndex == FailAtShotIndex)
                    {
                        OutErrorCode = ErrorCodes::ERR_CAPTURE_FAILED;
                        OutErrorMessage = FString::Printf(
                            TEXT("stub capture refused shot %d"), ShotIndex);
                        return false;
                    }
                }

                OutCapture.Filename = Frame.Filename;
                OutCapture.Width = Frame.Width;
                OutCapture.Height = Frame.Height;
                OutCapture.Path = bWarmupFrame
                    ? WarmupPath
                    : FString::Printf(TEXT("/stub/%s"), *Frame.Filename);
                OutCapture.EffectiveLocation = EffectiveLocationOverride.Get(Frame.Location);
                OutCapture.EffectiveRotation = EffectiveRotationOverride.Get(Frame.Rotation);
                return true;
            };
        }
    };

    // A pose looking along +X from 1000 cm back, so a 100 cm bounding sphere at the origin is well
    // inside a default 50-degree frame. The numbers are chosen so neither verdict is marginal.
    PinWrightPoseCapture::FCameraPose PoseListTestPoseAimedAtOrigin(int32 Index)
    {
        PinWrightPoseCapture::FCameraPose Pose;
        Pose.Location = FVector(-1000.0, 0.0, 0.0);
        Pose.Rotation = FRotator(0.0, 0.0, 0.0);
        Pose.Filename = FString::Printf(TEXT("PoseListTest_shot%02d.png"), Index);
        return Pose;
    }

    // A pose list whose every entry asks for a distinct instant. The times are exactly
    // representable in binary, so the comparisons below are exact rather than tolerant.
    PinWrightPoseCapture::FPoseListCaptureRequest PoseListTestTimedRequest(int32 PoseCount)
    {
        PinWrightPoseCapture::FPoseListCaptureRequest Request;
        Request.FilenamePrefix = TEXT("PoseListTest");
        Request.Subdirectory = TEXT("PoseListTest");
        for (int32 Index = 0; Index < PoseCount; ++Index)
        {
            PinWrightPoseCapture::FCameraPose Pose = PoseListTestPoseAimedAtOrigin(Index);
            Pose.SubjectTimeSeconds = 0.25f * static_cast<float>(Index + 1);
            Request.Poses.Add(MoveTemp(Pose));
        }
        return Request;
    }
}

// ---- the subject time driver ----

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPoseListCaptureTimeSetterOncePerPoseTest,
    "PinWright.render.pose_list.TimeSetterRunsOncePerPose",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPoseListCaptureTimeSetterOncePerPoseTest::RunTest(const FString& Parameters)
{
    PinWrightPoseCapture::FPoseListCaptureRequest Request = PoseListTestTimedRequest(3);
    Request.bWarmupShot = true;

    FPoseListTestStubViewport Stub;
    Request.SubjectTimeSetter = Stub.MakeTimeSetter();

    PinWrightPoseCapture::FPoseListCaptureOutput Output;
    FString ErrorCode;
    FString ErrorMessage;
    if (!TestTrue(TEXT("the pose set captured"), PinWrightPoseCapture::RunPoseListCapture(
            Request, Stub.MakeCapturer(), Output, ErrorCode, ErrorMessage)))
    {
        AddError(FString::Printf(TEXT("RunPoseListCapture failed: %s / %s"),
            *ErrorCode, *ErrorMessage));
        return false;
    }

    // PRECONDITION, not decoration. If the warm-up frame never ran, the invocation count below
    // would read 3 for the opposite reason and the test would pass while proving nothing.
    TestTrue(TEXT("the warm-up shot was taken"), Output.bWarmupShotTaken);
    TestEqual(TEXT("frames drawn: one warm-up plus one per pose"), Stub.Frames.Num(), 4);
    TestTrue(TEXT("the first frame drawn was the warm-up"), Stub.Frames[0].bWarmupFrame);

    // THE CONTRACT: once per pose, not once per frame.
    TestEqual(TEXT("the subject time setter ran once per pose"),
        Stub.SubjectTimes.Num(), Request.Poses.Num());
    TestEqual(TEXT("the setter was not entered a fourth time for the warm-up"),
        Stub.SetterCalls, Request.Poses.Num());
    TestEqual(TEXT("the set reports the same number of applications"),
        Output.SubjectTimesApplied, Request.Poses.Num());
    TestTrue(TEXT("the set reports that it had a time axis"), Output.bSubjectTimeDriven);

    // ORDER: pose 0's instant is applied BEFORE the throwaway frame, so the warm-up pages in the
    // content the set actually shows, at the moment it shows it.
    TestEqual(TEXT("the warm-up frame was drawn after pose 0's instant was applied"),
        Stub.Frames[0].SubjectTimeApplicationsBefore, 1);
    for (int32 Index = 0; Index < Request.Poses.Num(); ++Index)
    {
        TestEqual(*FString::Printf(TEXT("shot %d was drawn at its own instant"), Index),
            Stub.Frames[Index + 1].SubjectTimeApplicationsBefore, Index + 1);
        TestEqual(*FString::Printf(TEXT("instant %d is the pose's own time"), Index),
            Stub.SubjectTimes[Index], 0.25 * static_cast<double>(Index + 1), 0.0);
    }

    // The set-level report says what the driver did, and only on a set that had a driver.
    const TSharedPtr<FJsonObject> PoseSet = PinWrightPoseCapture::MakePoseSetInfoObject(Output);
    if (TestTrue(TEXT("poseSet reports subjectTimesApplied"),
            PoseSet->HasField(TEXT("subjectTimesApplied"))))
    {
        TestEqual(TEXT("poseSet.subjectTimesApplied"),
            static_cast<int32>(PoseSet->GetNumberField(TEXT("subjectTimesApplied"))), 3);
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPoseListCaptureNoTimeAxisIsSilentTest,
    "PinWright.render.pose_list.ASetWithoutTimesReportsNoTimeFields",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPoseListCaptureNoTimeAxisIsSilentTest::RunTest(const FString& Parameters)
{
    // The other direction of the same contract: seven of the eight capture verbs have no time axis
    // at all, and a `subjectTimesApplied: 0` on every one of them would be a field that never says
    // anything -- the silent-absence trap this plan's own risk list names.
    PinWrightPoseCapture::FPoseListCaptureRequest Request;
    Request.FilenamePrefix = TEXT("PoseListTest");
    Request.Poses.Add(PoseListTestPoseAimedAtOrigin(0));
    Request.bWarmupShot = false;

    FPoseListTestStubViewport Stub;
    PinWrightPoseCapture::FPoseListCaptureOutput Output;
    FString ErrorCode;
    FString ErrorMessage;
    TestTrue(TEXT("the pose set captured"), PinWrightPoseCapture::RunPoseListCapture(
        Request, Stub.MakeCapturer(), Output, ErrorCode, ErrorMessage));

    TestEqual(TEXT("the setter was never entered"), Stub.SetterCalls, 0);
    TestFalse(TEXT("the set reports no time axis"), Output.bSubjectTimeDriven);
    TestEqual(TEXT("no application was counted"), Output.SubjectTimesApplied, 0);

    const TSharedPtr<FJsonObject> PoseSet = PinWrightPoseCapture::MakePoseSetInfoObject(Output);
    TestFalse(TEXT("poseSet carries no subjectTimesApplied"),
        PoseSet->HasField(TEXT("subjectTimesApplied")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPoseListCaptureTimeWithoutSetterRefusedTest,
    "PinWright.render.pose_list.PoseTimeWithoutASetterIsRefused",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPoseListCaptureTimeWithoutSetterRefusedTest::RunTest(const FString& Parameters)
{
    PinWrightPoseCapture::FPoseListCaptureRequest Request = PoseListTestTimedRequest(2);
    Request.bWarmupShot = true;
    // Deliberately no SubjectTimeSetter: the caller asked for an instant nothing can reach.

    FPoseListTestStubViewport Stub;
    PinWrightPoseCapture::FPoseListCaptureOutput Output;
    FString ErrorCode;
    FString ErrorMessage;
    TestFalse(TEXT("the set is refused"), PinWrightPoseCapture::RunPoseListCapture(
        Request, Stub.MakeCapturer(), Output, ErrorCode, ErrorMessage));

    TestEqual(TEXT("the refusal is INVALID_ARGUMENT"),
        ErrorCode, FString(ErrorCodes::ERR_INVALID_ARGUMENT));
    // Naming the pose is what makes the message actionable on a 24-shot set.
    TestTrue(TEXT("the message names the pose index"), ErrorMessage.Contains(TEXT("Pose 0")));
    TestTrue(TEXT("the message names what is missing"),
        ErrorMessage.Contains(TEXT("subject time setter")));

    // THE POINT OF THE REFUSAL: no pixels were drawn at the wrong instant, not even the throwaway
    // frame. A refusal that fired after the set had been captured would be a warning, not a gate.
    TestEqual(TEXT("no frame was drawn"), Stub.Frames.Num(), 0);
    TestEqual(TEXT("nothing was captured"), Output.Captures.Num(), 0);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPoseListCaptureTimeFailureKeepsPartialSetTest,
    "PinWright.render.pose_list.TimeSetterFailureKeepsWhatWasCaptured",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPoseListCaptureTimeFailureKeepsPartialSetTest::RunTest(const FString& Parameters)
{
    PinWrightPoseCapture::FPoseListCaptureRequest Request = PoseListTestTimedRequest(3);
    Request.bWarmupShot = false;

    FPoseListTestStubViewport Stub;
    // Refuse the third application, i.e. pose 2, after two shots are already on disk.
    Stub.FailSetterAtCall = 2;
    Request.SubjectTimeSetter = Stub.MakeTimeSetter();

    PinWrightPoseCapture::FPoseListCaptureOutput Output;
    FString ErrorCode;
    FString ErrorMessage;
    TestFalse(TEXT("the set fails"), PinWrightPoseCapture::RunPoseListCapture(
        Request, Stub.MakeCapturer(), Output, ErrorCode, ErrorMessage));

    // The setter's own refusal reaches the caller verbatim -- the primitive does not overwrite it
    // with a code of its own.
    TestEqual(TEXT("the setter's error code is propagated"),
        ErrorCode, FString(ErrorCodes::ERR_CAPTURE_FAILED));
    TestEqual(TEXT("the setter's message is propagated"),
        ErrorMessage, FString(TEXT("stub subject time setter refused")));

    // Partial progress survives, in every parallel array.
    TestEqual(TEXT("two shots were captured before the failure"), Output.Captures.Num(), 2);
    TestEqual(TEXT("the request array stayed parallel"), Output.Requests.Num(), 2);
    TestEqual(TEXT("the pose array stayed parallel"), Output.Poses.Num(), 2);
    TestEqual(TEXT("the framing array stayed parallel"), Output.Framings.Num(), 2);
    TestEqual(TEXT("only two applications were counted"), Output.SubjectTimesApplied, 2);
    TestEqual(TEXT("the third frame was never drawn"), Stub.Frames.Num(), 2);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPoseListCaptureTruncatedPosesAreNotDrivenTest,
    "PinWright.render.pose_list.TruncatedPosesNeitherDriveTimeNorDraw",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPoseListCaptureTruncatedPosesAreNotDrivenTest::RunTest(const FString& Parameters)
{
    PinWrightPoseCapture::FPoseListCaptureRequest Request = PoseListTestTimedRequest(5);
    Request.bWarmupShot = false;
    Request.MaxPoses = 2;

    FPoseListTestStubViewport Stub;
    Request.SubjectTimeSetter = Stub.MakeTimeSetter();

    PinWrightPoseCapture::FPoseListCaptureOutput Output;
    FString ErrorCode;
    FString ErrorMessage;
    TestTrue(TEXT("the truncated set still captures"), PinWrightPoseCapture::RunPoseListCapture(
        Request, Stub.MakeCapturer(), Output, ErrorCode, ErrorMessage));

    TestEqual(TEXT("two poses were captured"), Output.Captures.Num(), 2);
    TestEqual(TEXT("three poses were truncated"), Output.PosesTruncated, 3);
    // A dropped pose must not advance the subject: advancing a simulation for a frame nobody
    // photographs is both a cost and a state change the response never mentions.
    TestEqual(TEXT("the setter ran only for the captured poses"), Stub.SetterCalls, 2);
    TestEqual(TEXT("subjectTimesApplied counts captured poses only"), Output.SubjectTimesApplied, 2);

    const TSharedPtr<FJsonObject> PoseSet = PinWrightPoseCapture::MakePoseSetInfoObject(Output);
    TestTrue(TEXT("the truncation is still reported"),
        PoseSet->HasField(TEXT("truncationWarning")));
    return true;
}

// ---- framing, measured against the pose the renderer resolved to ----

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPoseListCaptureFramingUsesEffectivePoseTest,
    "PinWright.render.pose_list.FramingIsMeasuredAgainstTheEffectivePose",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPoseListCaptureFramingUsesEffectivePoseTest::RunTest(const FString& Parameters)
{
    // ONE request, run twice. Nothing about what was asked for changes between the two runs; the
    // only difference is whether the viewport honoured the aim. If framing were measured off the
    // request -- the defect this asserts against -- both runs would return the same verdict.
    PinWrightPoseCapture::FPoseListCaptureRequest Request;
    Request.FilenamePrefix = TEXT("PoseListTest");
    Request.Poses.Add(PoseListTestPoseAimedAtOrigin(0));
    Request.bWarmupShot = false;
    Request.BoundsOrigin = FVector::ZeroVector;
    Request.BoundsRadius = 100.0;

    // Run A: the viewport aims where it was asked.
    FPoseListTestStubViewport HonouredStub;
    PinWrightPoseCapture::FPoseListCaptureOutput Honoured;
    FString ErrorCode;
    FString ErrorMessage;
    if (!TestTrue(TEXT("the honoured set captured"), PinWrightPoseCapture::RunPoseListCapture(
            Request, HonouredStub.MakeCapturer(), Honoured, ErrorCode, ErrorMessage)))
    {
        AddError(FString::Printf(TEXT("run A failed: %s / %s"), *ErrorCode, *ErrorMessage));
        return false;
    }

    // Run B: the viewport refuses the aim and points the camera the other way -- exactly what an
    // orbit-mode preview viewport does with a location whose direction it discards.
    FPoseListTestStubViewport RefusedStub;
    RefusedStub.EffectiveRotationOverride = FRotator(0.0, 180.0, 0.0);
    PinWrightPoseCapture::FPoseListCaptureOutput Refused;
    if (!TestTrue(TEXT("the refused set still captured"), PinWrightPoseCapture::RunPoseListCapture(
            Request, RefusedStub.MakeCapturer(), Refused, ErrorCode, ErrorMessage)))
    {
        AddError(FString::Printf(TEXT("run B failed: %s / %s"), *ErrorCode, *ErrorMessage));
        return false;
    }

    // PRECONDITION: the two runs really were asked for the same thing. Without this the differing
    // verdicts below could come from a differing request and prove nothing.
    if (!TestEqual(TEXT("both runs captured one shot"), Honoured.Captures.Num(), 1)
        || !TestEqual(TEXT("both runs captured one shot"), Refused.Captures.Num(), 1))
    {
        return false;
    }
    TestEqual(TEXT("the requested camera location is identical"),
        Refused.Requests[0].Location, Honoured.Requests[0].Location, 0.0f);
    TestEqual(TEXT("the requested camera rotation is identical"),
        Refused.Requests[0].Rotation, Honoured.Requests[0].Rotation, 0.0f);

    // The verdicts differ, and each is the right one for the pose the pixels actually show.
    TestTrue(TEXT("run A evaluated framing"), Honoured.Framings[0].bEvaluated);
    TestTrue(TEXT("run A: the subject is in frame"), Honoured.Framings[0].bBoundsInFrame);
    TestEqual(TEXT("run A: no shot is out of frame"), Honoured.PosesOutOfFrame, 0);

    TestTrue(TEXT("run B evaluated framing"), Refused.Framings[0].bEvaluated);
    TestFalse(TEXT("run B: the subject is provably NOT in frame"),
        Refused.Framings[0].bBoundsInFrame);
    TestTrue(TEXT("run B: the subject is behind the camera"), Refused.Framings[0].bBehindCamera);
    TestEqual(TEXT("run B: one shot is out of frame"), Refused.PosesOutOfFrame, 1);

    // The per-shot `framing` block follows the verdict, warning only on the miss.
    const TSharedPtr<FJsonObject> HonouredShot = MakeShared<FJsonObject>();
    PinWrightPoseCapture::AddPoseFramingField(Honoured, 0, HonouredShot);
    const TSharedPtr<FJsonObject> RefusedShot = MakeShared<FJsonObject>();
    PinWrightPoseCapture::AddPoseFramingField(Refused, 0, RefusedShot);

    if (TestTrue(TEXT("run A emits a framing block"), HonouredShot->HasField(TEXT("framing")))
        && TestTrue(TEXT("run B emits a framing block"), RefusedShot->HasField(TEXT("framing"))))
    {
        const TSharedPtr<FJsonObject>& HonouredFraming =
            HonouredShot->GetObjectField(TEXT("framing"));
        const TSharedPtr<FJsonObject>& RefusedFraming = RefusedShot->GetObjectField(TEXT("framing"));
        TestTrue(TEXT("run A: framing.boundsInFrame is true"),
            HonouredFraming->GetBoolField(TEXT("boundsInFrame")));
        TestFalse(TEXT("run B: framing.boundsInFrame is false"),
            RefusedFraming->GetBoolField(TEXT("boundsInFrame")));
        // The absence assertion that already guards this block elsewhere: no warning on a framed
        // capture.
        TestFalse(TEXT("run A carries no framingWarning"),
            HonouredFraming->HasField(TEXT("framingWarning")));
        TestTrue(TEXT("run B carries a framingWarning"),
            RefusedFraming->HasField(TEXT("framingWarning")));
    }

    // The set-level counts, and the set-level warning under its own name -- `outOfFrameWarning`,
    // never a second `framingWarning`.
    const TSharedPtr<FJsonObject> RefusedPoseSet =
        PinWrightPoseCapture::MakePoseSetInfoObject(Refused);
    TestEqual(TEXT("poseSet.posesFramingEvaluated"),
        static_cast<int32>(RefusedPoseSet->GetNumberField(TEXT("posesFramingEvaluated"))), 1);
    TestEqual(TEXT("poseSet.posesOutOfFrame"),
        static_cast<int32>(RefusedPoseSet->GetNumberField(TEXT("posesOutOfFrame"))), 1);
    TestTrue(TEXT("poseSet carries outOfFrameWarning"),
        RefusedPoseSet->HasField(TEXT("outOfFrameWarning")));
    TestFalse(TEXT("poseSet does not reuse the per-shot framingWarning name"),
        RefusedPoseSet->HasField(TEXT("framingWarning")));

    const TSharedPtr<FJsonObject> HonouredPoseSet =
        PinWrightPoseCapture::MakePoseSetInfoObject(Honoured);
    TestFalse(TEXT("a framed set carries no outOfFrameWarning"),
        HonouredPoseSet->HasField(TEXT("outOfFrameWarning")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPoseListCaptureNoBoundsNoFramingTest,
    "PinWright.render.pose_list.NoBoundsMeansNoFramingBlock",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPoseListCaptureNoBoundsNoFramingTest::RunTest(const FString& Parameters)
{
    // A set with no subject has nothing to be in frame. An `evaluated: false` block on every shot
    // of every verb that frames nothing is the empty-block noise the plan's block-parity rule
    // forbids -- present only when it has something to say.
    PinWrightPoseCapture::FPoseListCaptureRequest Request;
    Request.FilenamePrefix = TEXT("PoseListTest");
    Request.Poses.Add(PoseListTestPoseAimedAtOrigin(0));
    Request.bWarmupShot = false;
    // BoundsRadius left at its default 0.

    FPoseListTestStubViewport Stub;
    PinWrightPoseCapture::FPoseListCaptureOutput Output;
    FString ErrorCode;
    FString ErrorMessage;
    TestTrue(TEXT("the set captured"), PinWrightPoseCapture::RunPoseListCapture(
        Request, Stub.MakeCapturer(), Output, ErrorCode, ErrorMessage));

    TestFalse(TEXT("no framing was requested"), Output.bFramingRequested);
    if (TestEqual(TEXT("the framing array is still parallel"), Output.Framings.Num(), 1))
    {
        TestFalse(TEXT("the verdict is unevaluated"), Output.Framings[0].bEvaluated);
    }

    const TSharedPtr<FJsonObject> ShotObj = MakeShared<FJsonObject>();
    PinWrightPoseCapture::AddPoseFramingField(Output, 0, ShotObj);
    TestFalse(TEXT("no framing block is written"), ShotObj->HasField(TEXT("framing")));

    const TSharedPtr<FJsonObject> PoseSet = PinWrightPoseCapture::MakePoseSetInfoObject(Output);
    TestFalse(TEXT("poseSet carries no posesFramingEvaluated"),
        PoseSet->HasField(TEXT("posesFramingEvaluated")));
    TestFalse(TEXT("poseSet carries no posesOutOfFrame"),
        PoseSet->HasField(TEXT("posesOutOfFrame")));
    return true;
}

// ---- the warm-up frame and its report ----

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPoseListCaptureWarmupIsNotAShotTest,
    "PinWright.render.pose_list.WarmupFrameIsNotOneOfTheShots",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPoseListCaptureWarmupIsNotAShotTest::RunTest(const FString& Parameters)
{
    PinWrightPoseCapture::FPoseListCaptureRequest Request;
    Request.FilenamePrefix = TEXT("PoseListTest");
    Request.Poses.Add(PoseListTestPoseAimedAtOrigin(0));
    Request.Poses.Add(PoseListTestPoseAimedAtOrigin(1));
    Request.bWarmupShot = true;

    FPoseListTestStubViewport Stub;
    PinWrightPoseCapture::FPoseListCaptureOutput Output;
    FString ErrorCode;
    FString ErrorMessage;
    TestTrue(TEXT("the set captured"), PinWrightPoseCapture::RunPoseListCapture(
        Request, Stub.MakeCapturer(), Output, ErrorCode, ErrorMessage));

    TestEqual(TEXT("three frames were drawn"), Stub.Frames.Num(), 3);
    TestEqual(TEXT("two shots are reported"), Output.Captures.Num(), 2);
    TestEqual(TEXT("the request array is parallel"), Output.Requests.Num(), 2);
    TestEqual(TEXT("the pose array is parallel"), Output.Poses.Num(), 2);
    TestEqual(TEXT("the framing array is parallel"), Output.Framings.Num(), 2);
    TestTrue(TEXT("the warm-up is reported"), Output.bWarmupShotTaken);

    // The throwaway frame is drawn at the first real pose, under its own filename, so it can never
    // overwrite a shot the caller asked for.
    TestTrue(TEXT("the warm-up drew the first pose"),
        Stub.Frames[0].Frame.Location.Equals(Request.Poses[0].Location, 0.0));
    const FString& WarmupFilename = Stub.Frames[0].Frame.Filename;
    TestFalse(TEXT("the warm-up has its own generated filename"), WarmupFilename.IsEmpty());
    TestTrue(TEXT("the warm-up filename retains the set prefix"),
        WarmupFilename.StartsWith(TEXT("PoseListTest_warmup_"))
            && WarmupFilename.EndsWith(TEXT(".png")));
    for (const PinWrightRenderCapture::FViewportCaptureRequest& ShotRequest : Output.Requests)
    {
        TestNotEqual(TEXT("no shot shares the warm-up filename"),
            ShotRequest.Filename, WarmupFilename);
    }

    // One pixel size for the whole set, warm-up included: a capture size that varies within a
    // session trips the FViewport::GetHitProxy hit-proxy assertion.
    for (const FPoseListTestDrawnFrame& Drawn : Stub.Frames)
    {
        TestEqual(TEXT("every frame is the set's width"), Drawn.Frame.Width, Request.Width);
        TestEqual(TEXT("every frame is the set's height"), Drawn.Frame.Height, Request.Height);
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPoseListCaptureWarmupFileIsDeletedTest,
    "PinWright.render.pose_list.WarmupFrameFileIsDeleted",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPoseListCaptureWarmupFileIsDeletedTest::RunTest(const FString& Parameters)
{
    // The one assertion here that needs the filesystem, and it needs nothing else: a real file is
    // written, the primitive is told the throwaway frame landed there, and the file is gone
    // afterwards. Asserting the flag alone would pass on a delete that never ran.
    const FString WarmupPath = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("PinWright"),
        TEXT("PoseListCaptureTest_warmup.png"));
    if (!TestTrue(TEXT("the fixture file was written"),
            FFileHelper::SaveStringToFile(TEXT("not a png"), *WarmupPath)))
    {
        // A fixture that could not be created is a test that measured nothing -- fail, never skip.
        AddError(FString::Printf(TEXT("could not write the warm-up fixture at %s"), *WarmupPath));
        return false;
    }
    TestTrue(TEXT("the fixture file exists before the capture"),
        IFileManager::Get().FileExists(*WarmupPath));

    PinWrightPoseCapture::FPoseListCaptureRequest Request;
    Request.FilenamePrefix = TEXT("PoseListTest");
    Request.Poses.Add(PoseListTestPoseAimedAtOrigin(0));
    Request.bWarmupShot = true;

    FPoseListTestStubViewport Stub;
    Stub.WarmupPath = WarmupPath;
    PinWrightPoseCapture::FPoseListCaptureOutput Output;
    FString ErrorCode;
    FString ErrorMessage;
    TestTrue(TEXT("the set captured"), PinWrightPoseCapture::RunPoseListCapture(
        Request, Stub.MakeCapturer(), Output, ErrorCode, ErrorMessage));

    TestTrue(TEXT("the warm-up shot is reported"), Output.bWarmupShotTaken);
    TestTrue(TEXT("the warm-up file is reported discarded"), Output.bWarmupShotDiscarded);
    TestFalse(TEXT("the file is actually gone"), IFileManager::Get().FileExists(*WarmupPath));

    const TSharedPtr<FJsonObject> PoseSet = PinWrightPoseCapture::MakePoseSetInfoObject(Output);
    TestTrue(TEXT("poseSet.warmupShotDiscarded is true"),
        PoseSet->GetBoolField(TEXT("warmupShotDiscarded")));
    TestFalse(TEXT("a discarded warm-up file raises no warning"),
        PoseSet->HasField(TEXT("warmupFileNotDeletedWarning")));

    // Belt and braces: if the delete had failed, the fixture would still be on disk.
    IFileManager::Get().Delete(*WarmupPath, /*RequireExists=*/false, /*EvenReadOnly=*/true,
        /*Quiet=*/true);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPoseListCaptureWarmupWarningIsUnambiguousTest,
    "PinWright.render.pose_list.WarmupFileWarningIsNamedForWhatItMeans",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPoseListCaptureWarmupWarningIsUnambiguousTest::RunTest(const FString& Parameters)
{
    // `warmupWarning` is ALREADY TAKEN, by MakeViewportInfoObject, inside `viewport.warmup`, where
    // it means "the frame never settled -- these pixels are mid-warm-up". This producer means "the
    // throwaway PNG is still on disk". Different parents, so the two never collide in one JSON
    // object, but one name for two unrelated facts misleads anyone grepping the tree and breaks any
    // caller that flattens the response. Both directions are asserted so a revert to the old name
    // fails here rather than passing quietly.
    PinWrightPoseCapture::FPoseListCaptureOutput Leftover;
    Leftover.PosesRequested = 1;
    Leftover.bWarmupShotTaken = true;
    Leftover.bWarmupShotDiscarded = false;
    Leftover.WarmupShotPath = TEXT("C:/Saved/Screenshots/PoseSet_warmup.png");

    const TSharedPtr<FJsonObject> LeftoverSet =
        PinWrightPoseCapture::MakePoseSetInfoObject(Leftover);
    TestFalse(TEXT("the ambiguous name is not used"), LeftoverSet->HasField(TEXT("warmupWarning")));
    FString Warning;
    if (TestTrue(TEXT("the unambiguous name is used"),
            LeftoverSet->TryGetStringField(TEXT("warmupFileNotDeletedWarning"), Warning)))
    {
        // The whole value of the warning is that the caller can go and remove the file.
        TestTrue(TEXT("the warning names the file"), Warning.Contains(Leftover.WarmupShotPath));
    }

    // Deleted: no warning at all.
    PinWrightPoseCapture::FPoseListCaptureOutput Discarded = Leftover;
    Discarded.bWarmupShotDiscarded = true;
    const TSharedPtr<FJsonObject> DiscardedSet =
        PinWrightPoseCapture::MakePoseSetInfoObject(Discarded);
    TestFalse(TEXT("a discarded frame raises no file warning"),
        DiscardedSet->HasField(TEXT("warmupFileNotDeletedWarning")));
    TestFalse(TEXT("and not the ambiguous name either"),
        DiscardedSet->HasField(TEXT("warmupWarning")));

    // No file was ever written: nothing to clean up, so nothing to warn about. The old condition
    // warned here about a leftover PNG that does not exist and could not name it.
    PinWrightPoseCapture::FPoseListCaptureOutput NoFile = Leftover;
    NoFile.WarmupShotPath.Reset();
    const TSharedPtr<FJsonObject> NoFileSet = PinWrightPoseCapture::MakePoseSetInfoObject(NoFile);
    TestFalse(TEXT("no file means no leftover-file warning"),
        NoFileSet->HasField(TEXT("warmupFileNotDeletedWarning")));
    return true;
}
