// Copyright (c) 2026 Alexander Penkin. MIT License.

// The crossing rule that turns a camera list plus a list of instants into a pose list:
// PinWrightSubjectTimeSeries::CrossCamerasWithInstants, and what the pose-list primitive then does
// with the layout it produces.
//
// WHY A NEW FILE. Tests/Render/TestPoseListCaptureSubjectTime.cpp is the regression floor for the
// primitive's own time driver and must pass UNMODIFIED; this file asserts the layer above it -- who
// decides WHICH poses carry an instant -- so a new assertion goes in a new file.
//
// WHY THESE NEED NO VIEWPORT, NO WORLD, NO GPU AND NO ASSET. The crossing is a pure function over
// two arrays, and RunPoseListCapture is the production sequence with the one frame-drawing step
// injected as a closure. So every assertion below runs against shipping code with a stub in place
// of the draw, and none of them can take a conditional-skip path and report success without having
// asserted anything -- board ticket B-test-skips-assertions-silently fired on exactly this cluster
// of verbs, where a test skipped its only substantive assertions in 3 of 3 runs because another
// process held the GPU and the suite counted it green.
//
// WHAT IS DEFENDED, and what it costs when it breaks:
//
//  * THE SUBJECT IS DRIVEN ONCE PER INSTANT, NOT ONCE PER POSE. This is the whole reason the
//    crossing exists. UNiagaraComponent::AdvanceSimulation is preceded by a ResetSystem, and with
//    system determinism off the per-instance seed is FMath::Rand() on every reset
//    (NiagaraSystemInstance.cpp:894). Re-driving per pose would therefore make the six sides of
//    "t = 0.5 s" six DIFFERENT explosions that merely share a number -- six individually valid
//    frames that cannot be compared, which is the one thing a multi-angle set exists to allow.
//    Nothing in the image says which of the two happened, so only a test can.
//  * THE PER-CAMERA TABLES ARE INDEXED BY CAMERA, NOT BY SHOT. Once instants repeat the camera
//    list, shot index and camera index stop being the same number. A verb that kept indexing by
//    shot would report shot 4 of a 2x3 set with camera 4's angles -- silently wrong metadata on a
//    frame that is otherwise perfect.
//  * EVERY SHOT GETS ITS OWN FILENAME. The capture util's auto-name carries a one-second timestamp
//    and a capture takes ~60 ms, so an inherited filename means N response entries and ONE PNG on
//    disk. That has already produced three "independent" readings of a single image here.

#include "Misc/AutomationTest.h"

#include "Handlers/ErrorCodes.h"
#include "Handlers/Render/PoseListCapture.h"
#include "Handlers/Render/SubjectTimeSeries.h"

#include "Containers/Set.h"

namespace
{
    // Prefixed for the same reason as every other helper under Tests/Render: anonymous namespaces
    // in one Unity translation unit merge, so a bare name a sibling file also uses is a latent ODR
    // clash rather than a compile error.
    struct PWTimeSeriesDrawnFrame
    {
        FString Filename;
        bool bWarmupFrame = false;
        // How many subject-time drives had completed when this frame was drawn. This is the field
        // that separates "driven once per instant and HELD" from "driven once and then drifted":
        // every shot of instant N must be drawn with exactly N+1 drives behind it.
        int32 DrivesBefore = 0;
    };

    struct PWTimeSeriesStub
    {
        TArray<PWTimeSeriesDrawnFrame> Frames;
        TArray<double> DrivenTimes;
        int32 SetterCalls = 0;
        int32 ShotsDrawn = 0;

        PinWrightPoseCapture::FSubjectTimeSetter MakeTimeSetter()
        {
            return [this](double TimeSeconds, FString& OutErrCode, FString& OutErrMsg) -> bool
            {
                ++SetterCalls;
                DrivenTimes.Add(TimeSeconds);
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
                PWTimeSeriesDrawnFrame Drawn;
                Drawn.Filename = Frame.Filename;
                Drawn.bWarmupFrame = bWarmupFrame;
                Drawn.DrivesBefore = DrivenTimes.Num();
                Frames.Add(Drawn);
                if (!bWarmupFrame)
                {
                    ++ShotsDrawn;
                }
                OutCapture.Filename = Frame.Filename;
                OutCapture.Width = Frame.Width;
                OutCapture.Height = Frame.Height;
                // Empty for the warm-up frame so the primitive never reaches the filesystem to
                // delete it: these tests must not touch disk.
                OutCapture.Path = bWarmupFrame ? FString() : FString::Printf(TEXT("/stub/%s"), *Frame.Filename);
                OutCapture.EffectiveLocation = Frame.Location;
                OutCapture.EffectiveRotation = Frame.Rotation;
                return true;
            };
        }
    };

    // Three distinguishable cameras. The filenames they arrive with are deliberately IDENTICAL, so
    // a crossing that inherited them instead of rewriting them produces six identical names and the
    // uniqueness assertion below fails rather than passing by luck.
    TArray<PinWrightPoseCapture::FCameraPose> PWTimeSeriesCameras(int32 Count)
    {
        TArray<PinWrightPoseCapture::FCameraPose> Cameras;
        for (int32 Index = 0; Index < Count; ++Index)
        {
            PinWrightPoseCapture::FCameraPose Pose;
            Pose.Location = FVector(-1000.0, 100.0 * Index, 0.0);
            Pose.Rotation = FRotator(0.0, 0.0, 0.0);
            Pose.Filename = TEXT("Inherited.png");
            Cameras.Add(MoveTemp(Pose));
        }
        return Cameras;
    }

    // Exactly representable in binary, so every comparison below is exact rather than tolerant.
    const TArray<double> PWTimeSeriesInstants = {0.0, 0.5};
}

// ============================================================================
// The crossing rule itself.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSubjectTimeSeriesOneDrivePerInstantTest,
    "PinWright.render.subject_time_series.InstantMarksTheFirstCameraOfItsGroupOnly",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSubjectTimeSeriesOneDrivePerInstantTest::RunTest(const FString& Parameters)
{
    TArray<PinWrightPoseCapture::FCameraPose> OutPoses;
    TArray<int32> InstantIndex;
    TArray<int32> CameraIndex;
    PinWrightSubjectTimeSeries::CrossCamerasWithInstants(
        PWTimeSeriesCameras(3), PWTimeSeriesInstants, TEXT("Stem"), TArray<FString>(),
        OutPoses, InstantIndex, CameraIndex);

    // UNABLE TO FAIL IF the crossing produced fewer poses than instants x cameras, because then
    // "only two poses carry a time" would be true for the trivial reason that only two poses exist.
    // This assertion is what forces the next one to mean what it says.
    TestEqual(TEXT("2 instants x 3 cameras produces 6 poses"), OutPoses.Num(), 6);

    int32 TimedPoses = 0;
    TArray<int32> TimedAt;
    for (int32 Index = 0; Index < OutPoses.Num(); ++Index)
    {
        if (OutPoses[Index].SubjectTimeSeconds.IsSet())
        {
            ++TimedPoses;
            TimedAt.Add(Index);
        }
    }

    // UNABLE TO FAIL IF the count were asserted against OutPoses.Num() instead of against the
    // instant count -- that comparison is true both when the rule holds and when every pose carries
    // a time, which is the exact defect this test exists to catch. 2 and 6 are different numbers on
    // purpose, and the assertion above pins 6 so neither can drift into the other.
    TestEqual(TEXT("exactly one pose per INSTANT carries a time, not one per pose: re-driving per "
                   "pose would reset and reseed the simulation for every angle"),
        TimedPoses, PWTimeSeriesInstants.Num());

    // UNABLE TO FAIL IF only the COUNT were asserted: two timed poses at indices 1 and 4 would
    // satisfy a count check while driving the subject AFTER camera 0 of each group had already been
    // photographed at the previous instant. Position is the property, not quantity.
    if (TestEqual(TEXT("two poses carry a time"), TimedAt.Num(), 2))
    {
        TestEqual(TEXT("the first camera of instant 0 carries it"), TimedAt[0], 0);
        TestEqual(TEXT("the first camera of instant 1 carries it, i.e. pose 3 of a 3-camera group"),
            TimedAt[1], 3);
    }

    // UNABLE TO FAIL IF the values were not checked: a crossing that marked the right poses with
    // the WRONG instants would place every camera correctly and photograph the wrong moments.
    if (OutPoses.Num() == 6)
    {
        TestTrue(TEXT("pose 0 carries instant 0"),
            OutPoses[0].SubjectTimeSeconds.IsSet() && OutPoses[0].SubjectTimeSeconds.GetValue() == 0.0f);
        TestTrue(TEXT("pose 3 carries instant 1 (0.5 s), so the series runs in the order asked for"),
            OutPoses[3].SubjectTimeSeconds.IsSet() && OutPoses[3].SubjectTimeSeconds.GetValue() == 0.5f);
        // UNABLE TO FAIL IF only the marked poses were inspected. A camera pose copied from a
        // template that already carried a time would silently re-drive, and the count assertion
        // above would still pass if the template were the untimed one. Asserting the NEGATIVE on
        // every other pose is what closes that.
        for (const int32 Unmarked : {1, 2, 4, 5})
        {
            TestFalse(*FString::Printf(TEXT("pose %d carries NO time, so the subject is held rather "
                                            "than re-driven for it"), Unmarked),
                OutPoses[Unmarked].SubjectTimeSeconds.IsSet());
        }
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSubjectTimeSeriesIndexTablesTest,
    "PinWright.render.subject_time_series.CameraAndInstantIndexTablesStayParallel",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSubjectTimeSeriesIndexTablesTest::RunTest(const FString& Parameters)
{
    TArray<PinWrightPoseCapture::FCameraPose> OutPoses;
    TArray<int32> InstantIndex;
    TArray<int32> CameraIndex;
    PinWrightSubjectTimeSeries::CrossCamerasWithInstants(
        PWTimeSeriesCameras(3), PWTimeSeriesInstants, TEXT("Stem"), TArray<FString>(),
        OutPoses, InstantIndex, CameraIndex);

    // UNABLE TO FAIL IF the tables were merely non-empty: a verb reads its per-camera angle table
    // through these, so a table of the right LENGTH carrying shot indices would look healthy and
    // report camera 4's azimuth for shot 4 of a set that only has 3 cameras.
    const TArray<int32> ExpectedCamera = {0, 1, 2, 0, 1, 2};
    const TArray<int32> ExpectedInstant = {0, 0, 0, 1, 1, 1};
    TestEqual(TEXT("the camera table is one entry per pose"), CameraIndex.Num(), 6);
    TestEqual(TEXT("the instant table is one entry per pose"), InstantIndex.Num(), 6);
    if (CameraIndex.Num() == 6 && InstantIndex.Num() == 6)
    {
        for (int32 Index = 0; Index < 6; ++Index)
        {
            // UNABLE TO FAIL IF this compared CameraIndex[i] to i: that identity holds for a
            // single-instant set and is precisely the assumption a time series breaks.
            TestEqual(*FString::Printf(TEXT("pose %d used camera %d, NOT camera %d"),
                          Index, ExpectedCamera[Index], Index),
                CameraIndex[Index], ExpectedCamera[Index]);
            TestEqual(*FString::Printf(TEXT("pose %d belongs to instant %d, so the layout is "
                                            "instant-major"), Index, ExpectedInstant[Index]),
                InstantIndex[Index], ExpectedInstant[Index]);
        }
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSubjectTimeSeriesFilenamesTest,
    "PinWright.render.subject_time_series.EveryShotGetsItsOwnFilename",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSubjectTimeSeriesFilenamesTest::RunTest(const FString& Parameters)
{
    TArray<PinWrightPoseCapture::FCameraPose> OutPoses;
    TArray<int32> InstantIndex;
    TArray<int32> CameraIndex;
    const TArray<FString> Tags = {TEXT("_az0_el0"), TEXT("_az90_el0"), TEXT("_az180_el0")};
    PinWrightSubjectTimeSeries::CrossCamerasWithInstants(
        PWTimeSeriesCameras(3), PWTimeSeriesInstants, TEXT("Stem"), Tags,
        OutPoses, InstantIndex, CameraIndex);

    TSet<FString> Unique;
    for (const PinWrightPoseCapture::FCameraPose& Pose : OutPoses)
    {
        Unique.Add(Pose.Filename);
    }
    // UNABLE TO FAIL IF the cameras arrived with distinct filenames: they all arrive as
    // "Inherited.png" (PWTimeSeriesCameras), so a crossing that inherited rather than rewrote
    // yields ONE unique name and this reads 1, not 6.
    TestEqual(TEXT("6 poses produce 6 distinct filenames, so 6 PNGs land on disk rather than one "
                   "file overwritten 6 times"),
        Unique.Num(), 6);

    if (OutPoses.Num() == 6)
    {
        // UNABLE TO FAIL IF uniqueness alone were checked: names unique only by shot index would
        // pass that while giving a reviewer no way to tell which frame is which moment. The instant
        // has to be IN the name.
        TestTrue(TEXT("the instant is spelled in the filename in whole milliseconds"),
            OutPoses[0].Filename.Contains(TEXT("_t0000ms")));
        TestTrue(TEXT("0.5 s reads as t0500ms, so the names sort into time order"),
            OutPoses[3].Filename.Contains(TEXT("_t0500ms")));
        TestTrue(TEXT("the caller's camera tag survives, so an angle is still identifiable"),
            OutPoses[4].Filename.Contains(TEXT("_az90_el0")));
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSubjectTimeSeriesEmptyInputsTest,
    "PinWright.render.subject_time_series.EmptyInputProducesNoPoses",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSubjectTimeSeriesEmptyInputsTest::RunTest(const FString& Parameters)
{
    TArray<PinWrightPoseCapture::FCameraPose> OutPoses;
    TArray<int32> InstantIndex;
    TArray<int32> CameraIndex;

    // Seeded with junk so "the function cleared its outputs" is a measured fact rather than the
    // default state of a fresh array. UNABLE TO FAIL IF the arrays started empty: the assertions
    // would then hold for a function that never touched them at all.
    OutPoses.Add(PinWrightPoseCapture::FCameraPose());
    InstantIndex.Add(7);
    CameraIndex.Add(7);

    PinWrightSubjectTimeSeries::CrossCamerasWithInstants(
        PWTimeSeriesCameras(3), TArray<double>(), TEXT("Stem"), TArray<FString>(),
        OutPoses, InstantIndex, CameraIndex);
    TestEqual(TEXT("no instants means no poses, and the caller's stale output is cleared"),
        OutPoses.Num(), 0);
    TestEqual(TEXT("and the index tables are cleared with them"), InstantIndex.Num(), 0);

    OutPoses.Add(PinWrightPoseCapture::FCameraPose());
    PinWrightSubjectTimeSeries::CrossCamerasWithInstants(
        TArray<PinWrightPoseCapture::FCameraPose>(), PWTimeSeriesInstants, TEXT("Stem"),
        TArray<FString>(), OutPoses, InstantIndex, CameraIndex);
    TestEqual(TEXT("no cameras means no poses"), OutPoses.Num(), 0);
    TestEqual(TEXT("and no camera table"), CameraIndex.Num(), 0);
    return true;
}

// ============================================================================
// What the shipping pose-list primitive does with that layout.
//
// The crossing tests above prove the pose list is MARKED correctly. These prove the marking has the
// effect it was designed for once the production sequence runs over it -- which is a separate
// claim, and the one that actually reaches a Niagara simulation.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSubjectTimeSeriesDrivesOncePerInstantTest,
    "PinWright.render.subject_time_series.PrimitiveDrivesTheSubjectOncePerInstant",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSubjectTimeSeriesDrivesOncePerInstantTest::RunTest(const FString& Parameters)
{
    PinWrightPoseCapture::FPoseListCaptureRequest Request;
    Request.FilenamePrefix = TEXT("TimeSeriesTest");
    Request.Subdirectory = TEXT("TimeSeriesTest");
    TArray<int32> InstantIndex;
    TArray<int32> CameraIndex;
    PinWrightSubjectTimeSeries::CrossCamerasWithInstants(
        PWTimeSeriesCameras(3), PWTimeSeriesInstants, TEXT("Stem"), TArray<FString>(),
        Request.Poses, InstantIndex, CameraIndex);

    PWTimeSeriesStub Stub;
    Request.SubjectTimeSetter = Stub.MakeTimeSetter();

    PinWrightPoseCapture::FPoseListCaptureOutput Result;
    FString ErrorCode;
    FString ErrorMessage;
    const bool bRan = PinWrightPoseCapture::RunPoseListCapture(
        Request, Stub.MakeCapturer(), Result, ErrorCode, ErrorMessage);

    // UNABLE TO FAIL IF the run were not asserted to have succeeded: every count below would read 0
    // on an early return, and "0 drives" would pass a naive "not once per pose" check.
    TestTrue(*FString::Printf(TEXT("the set runs (%s: %s)"), *ErrorCode, *ErrorMessage), bRan);
    TestEqual(TEXT("all six shots were drawn"), Stub.ShotsDrawn, 6);

    // UNABLE TO FAIL IF the expected value were 6: that is what a per-pose driver produces, and it
    // is the defect. 2 and 6 are the two outcomes this test separates, and ShotsDrawn is pinned at
    // 6 above so a set that silently shortened to two shots cannot make this pass for the wrong
    // reason.
    TestEqual(TEXT("the subject was driven ONCE PER INSTANT (2), not once per pose (6): a Niagara "
                   "advance resets and reseeds, so per-pose driving gives every angle a different "
                   "simulation"),
        Stub.SetterCalls, 2);
    TestEqual(TEXT("the primitive counts the same drives it performed"),
        Result.SubjectTimesApplied, 2);
    TestTrue(TEXT("and reports that the set had a time axis at all"), Result.bSubjectTimeDriven);

    // UNABLE TO FAIL IF only the drive COUNT were checked. Two drives in the wrong order -- 0.5 s
    // then 0.0 s -- would satisfy every count above while writing t0000ms onto the later frame.
    if (TestEqual(TEXT("two instants were driven"), Stub.DrivenTimes.Num(), 2))
    {
        TestEqual(TEXT("instant 0 was driven first"), Stub.DrivenTimes[0], 0.0);
        TestEqual(TEXT("instant 1 second, in the order the caller listed them"),
            Stub.DrivenTimes[1], 0.5);
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSubjectTimeSeriesHoldsAnInstantTest,
    "PinWright.render.subject_time_series.EveryCameraOfAnInstantSeesTheSameDrive",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSubjectTimeSeriesHoldsAnInstantTest::RunTest(const FString& Parameters)
{
    PinWrightPoseCapture::FPoseListCaptureRequest Request;
    Request.FilenamePrefix = TEXT("TimeSeriesTest");
    Request.Subdirectory = TEXT("TimeSeriesTest");
    TArray<int32> InstantIndex;
    TArray<int32> CameraIndex;
    PinWrightSubjectTimeSeries::CrossCamerasWithInstants(
        PWTimeSeriesCameras(3), PWTimeSeriesInstants, TEXT("Stem"), TArray<FString>(),
        Request.Poses, InstantIndex, CameraIndex);

    PWTimeSeriesStub Stub;
    Request.SubjectTimeSetter = Stub.MakeTimeSetter();

    PinWrightPoseCapture::FPoseListCaptureOutput Result;
    FString ErrorCode;
    FString ErrorMessage;
    const bool bRan = PinWrightPoseCapture::RunPoseListCapture(
        Request, Stub.MakeCapturer(), Result, ErrorCode, ErrorMessage);
    TestTrue(*FString::Printf(TEXT("the set runs (%s: %s)"), *ErrorCode, *ErrorMessage), bRan);

    // Shots only. The warm-up frame reuses pose 0 and is deliberately drawn AFTER instant 0's
    // drive, which is asserted separately below rather than folded in here.
    TArray<PWTimeSeriesDrawnFrame> Shots;
    int32 WarmupFrames = 0;
    for (const PWTimeSeriesDrawnFrame& Frame : Stub.Frames)
    {
        if (Frame.bWarmupFrame)
        {
            ++WarmupFrames;
        }
        else
        {
            Shots.Add(Frame);
        }
    }

    // UNABLE TO FAIL IF the warm-up frame were not separated out: it is drawn from pose 0 and would
    // otherwise shift every index by one and make the grouping assertion below read the wrong rows.
    TestEqual(TEXT("exactly one throwaway warm-up frame was drawn"), WarmupFrames, 1);
    if (!TestEqual(TEXT("six real shots were drawn"), Shots.Num(), 6))
    {
        return false;
    }

    // THE ORDERING CLAIM, which no count can make. Every shot of instant 0 must be drawn with
    // exactly ONE drive behind it and every shot of instant 1 with exactly TWO. A driver that
    // re-drove per pose would show 1,2,3,4,5,6; one that drove everything up front would show
    // 2,2,2,2,2,2; one that drove after drawing would show 0,...
    //
    // UNABLE TO FAIL IF this asserted only the first and last shot: 1,2,2,2,2,2 satisfies that and
    // means cameras 1 and 2 of instant 0 were photographed at the WRONG moment. Every row is
    // checked.
    const TArray<int32> ExpectedDrivesBefore = {1, 1, 1, 2, 2, 2};
    for (int32 Index = 0; Index < 6; ++Index)
    {
        TestEqual(*FString::Printf(
                      TEXT("shot %d was drawn with %d drive(s) behind it, so it shows instant %d and "
                           "shares that drive with the other cameras of its group"),
                      Index, ExpectedDrivesBefore[Index], ExpectedDrivesBefore[Index] - 1),
            Shots[Index].DrivesBefore, ExpectedDrivesBefore[Index]);
    }

    // UNABLE TO FAIL IF the warm-up frame's ordering were ignored: a warm-up drawn before instant 0
    // was applied pages in the wrong content, which is the whole reason the primitive drives pose 0
    // ahead of it.
    TestEqual(TEXT("the warm-up frame was drawn AFTER instant 0 was applied"),
        Stub.Frames.Num() > 0 ? Stub.Frames[0].DrivesBefore : -1, 1);
    return true;
}
