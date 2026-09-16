// Copyright (c) 2026 Alexander Penkin. MIT License.

// Tests for the SUBJECT COVERAGE DIFFERENTIAL: the arithmetic that counts changed pixels, and the
// sequence RunPoseListCapture runs around it.
//
// WHY THE MEASUREMENT IS DIFFERENTIAL AT ALL, because a reader who does not know this will try to
// replace it with something cheaper. `boundsInFrame` answers "could the subject's bounding sphere
// project into this frame", and that stays TRUE when the subject is a dot, when the floor occludes
// it, and when a Niagara system has no particles alive at the captured instant. A real capture of
// an empty preview scene came back with boundsInFrame:true, blank:false and litPixelFraction:1.0 --
// every published signal green over a picture containing nothing.
//
// A GEOMETRIC COVERAGE NUMBER DOES NOT FIX THAT, and the number is worth carrying here so nobody
// re-derives it: for one of those empty frames (bounds radius 173.2 at fit distance 464, fov 50)
// the projected disc is asin(173.2/464) = 21.9 degrees against a 25-degree half-fov, which is
// ~58% of the frame area. A geometric metric reports 58% coverage for an empty frame. The
// differential returns 0.000 for the same frame, which is what
// FPoseListCoverageEmptyFrameScoresZeroTest below asserts in unit form.
//
// WHY THESE NEED NO VIEWPORT, NO WORLD AND NO GPU. MeasureChangedPixelFraction is pure and takes
// two colour buffers, and RunPoseListCapture is the production sequence with the frame-drawing step
// injected as a closure. So nothing below can take a conditional-skip path and report success
// without having asserted anything -- board ticket B-test-skips-assertions-silently, which fired on
// this exact cluster of verbs.
//
// EVERY ASSERTION BELOW CARRIES AN "UNABLE TO FAIL IF" NOTE. Eight acceptance criteria have been
// replaced across this project for being unfailable, one of them inverted such that a correct
// implementation would have failed it. A test that asserts a coverage of 0.0 is exactly the shape
// that trap takes: 0.0 is also what a function that measures nothing returns, what an uninitialised
// out-parameter holds, and what a refusal that zeroes its output leaves behind. So each zero is
// asserted beside a non-zero measured on the same code path.

#include "Misc/AutomationTest.h"
#include "Handlers/Render/PoseListCapture.h"

#include "Handlers/ErrorCodes.h"
#include "Handlers/HandlerRegistration.h"
#include "Handlers/ParamSpec.h"

#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace
{
    // Prefixed for the same reason as every other helper in Tests/Render: anonymous namespaces in
    // one Unity translation unit merge, so a bare name a sibling file also uses is a latent ODR
    // clash rather than a compile error.

    // A flat buffer of one colour. The base is mid-grey rather than black so that a bug which
    // writes zeroes over the buffer shows up as a difference rather than blending into it.
    TArray<FColor> PoseListCoverageMakeBuffer(int32 PixelCount, FColor Fill = FColor(64, 64, 64, 255))
    {
        TArray<FColor> Buffer;
        Buffer.Init(Fill, PixelCount);
        return Buffer;
    }
}

// ---- the arithmetic ----

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPoseListCoverageCountsChangedPixelsTest,
    "PinWright.render.pose_list.CoverageCountsOnlyThePixelsThatChanged",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPoseListCoverageCountsChangedPixelsTest::RunTest(const FString& Parameters)
{
    // 400 pixels, 100 of them moved well past the threshold: the expected answer is 0.25, a value
    // that is neither 0 nor 1 nor the buffer size, so it cannot be produced by a stuck return, by a
    // count of everything, or by a count of nothing.
    const TArray<FColor> Reference = PoseListCoverageMakeBuffer(400);
    TArray<FColor> Subject = Reference;
    for (int32 Index = 0; Index < 100; ++Index)
    {
        Subject[Index] = FColor(200, 64, 64, 255);
    }

    // Seeded with a value the function must overwrite. UNABLE TO FAIL IF this were seeded with the
    // expected 0.25: a function that never wrote the out-parameter would then pass.
    double Fraction = -1.0;
    if (!TestTrue(TEXT("two same-sized buffers are comparable"),
            PinWrightPoseCapture::MeasureChangedPixelFraction(Reference, Subject, 8, Fraction)))
    {
        return false;
    }
    TestEqual(TEXT("100 changed pixels in 400 is 0.25"), Fraction, 0.25, 0.0);

    // THE COMPANION ZERO, measured on the same call shape. Without it the 0.25 above could come
    // from a counter that is simply always a quarter of the buffer, and the 0.000 this whole
    // measurement rests on would never be shown to be reachable.
    //
    // UNABLE TO FAIL IF the two buffers differed: they are the same object's copy, asserted equal
    // in length first, so a zero here means "identical inputs measured as no change" rather than
    // "the comparison never ran".
    TArray<FColor> Unchanged = Reference;
    TestEqual(TEXT("the unchanged buffer is the same length"), Unchanged.Num(), Reference.Num());
    double UnchangedFraction = -1.0;
    if (TestTrue(TEXT("identical buffers are comparable"),
            PinWrightPoseCapture::MeasureChangedPixelFraction(Reference, Unchanged, 8,
                UnchangedFraction)))
    {
        TestEqual(TEXT("identical buffers differ in no pixels"), UnchangedFraction, 0.0, 0.0);
    }

    // And the far end, so the scale is pinned at both extremes.
    TArray<FColor> AllChanged = PoseListCoverageMakeBuffer(400, FColor(255, 255, 255, 255));
    double AllChangedFraction = -1.0;
    if (TestTrue(TEXT("a wholly different buffer is comparable"),
            PinWrightPoseCapture::MeasureChangedPixelFraction(Reference, AllChanged, 8,
                AllChangedFraction)))
    {
        TestEqual(TEXT("a wholly different buffer is 1.0"), AllChangedFraction, 1.0, 0.0);
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPoseListCoverageThresholdIsStrictTest,
    "PinWright.render.pose_list.CoverageThresholdIsStrictlyGreaterThan",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPoseListCoverageThresholdIsStrictTest::RunTest(const FString& Parameters)
{
    // The threshold rides over encoder and temporal noise between two draws of the same pose. Its
    // boundary is asserted from BOTH sides in one test on purpose: a test that only shows
    // "difference 9 counts" passes just as well against a function that ignores the threshold
    // entirely and counts every non-zero difference.
    const TArray<FColor> Reference = PoseListCoverageMakeBuffer(100);

    // Exactly at the threshold: 64 -> 72 is a difference of 8, which must NOT count.
    TArray<FColor> AtThreshold = Reference;
    for (int32 Index = 0; Index < 50; ++Index)
    {
        AtThreshold[Index] = FColor(72, 64, 64, 255);
    }
    double AtFraction = -1.0;
    if (TestTrue(TEXT("the at-threshold buffer is comparable"),
            PinWrightPoseCapture::MeasureChangedPixelFraction(Reference, AtThreshold, 8, AtFraction)))
    {
        TestEqual(TEXT("a difference of exactly the threshold does not count"), AtFraction, 0.0, 0.0);
    }

    // One past it: 64 -> 73 is 9, which must count. UNABLE TO FAIL IF the two halves used
    // different pixel counts or different buffers -- they are the same 50 of the same 100, so the
    // only variable between the two answers is the one-unit change in the difference.
    TArray<FColor> PastThreshold = Reference;
    for (int32 Index = 0; Index < 50; ++Index)
    {
        PastThreshold[Index] = FColor(73, 64, 64, 255);
    }
    double PastFraction = -1.0;
    if (TestTrue(TEXT("the past-threshold buffer is comparable"),
            PinWrightPoseCapture::MeasureChangedPixelFraction(Reference, PastThreshold, 8,
                PastFraction)))
    {
        TestEqual(TEXT("a difference of one past the threshold counts"), PastFraction, 0.5, 0.0);
    }

    // ANY channel, not the first one. A green-only and a blue-only move must each count, or a
    // faint additive particle that never touches red would measure as an empty frame.
    TArray<FColor> GreenOnly = Reference;
    GreenOnly[0] = FColor(64, 200, 64, 255);
    TArray<FColor> BlueOnly = Reference;
    BlueOnly[0] = FColor(64, 64, 200, 255);
    double GreenFraction = -1.0;
    double BlueFraction = -1.0;
    PinWrightPoseCapture::MeasureChangedPixelFraction(Reference, GreenOnly, 8, GreenFraction);
    PinWrightPoseCapture::MeasureChangedPixelFraction(Reference, BlueOnly, 8, BlueFraction);
    TestEqual(TEXT("a green-only change counts"), GreenFraction, 0.01, 0.0);
    TestEqual(TEXT("a blue-only change counts"), BlueFraction, 0.01, 0.0);

    // ALPHA IS NOT A CHANNEL HERE. ForceOpaqueAlpha rewrites it to 255 on every readback before
    // this ever sees it, so an alpha term could only ever contribute zero -- and counting it would
    // tell a reader that a capture's alpha carries information, which it does not.
    //
    // UNABLE TO FAIL IF the alpha move were small: 255 -> 0 is the largest difference expressible,
    // so a zero here cannot be the threshold swallowing it.
    TArray<FColor> AlphaOnly = Reference;
    AlphaOnly[0] = FColor(64, 64, 64, 0);
    double AlphaFraction = -1.0;
    if (TestTrue(TEXT("the alpha-only buffer is comparable"),
            PinWrightPoseCapture::MeasureChangedPixelFraction(Reference, AlphaOnly, 8, AlphaFraction)))
    {
        TestEqual(TEXT("an alpha-only change does not count"), AlphaFraction, 0.0, 0.0);
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPoseListCoverageRefusalLeavesTheOutputAloneTest,
    "PinWright.render.pose_list.CoverageRefusalIsNotAZero",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPoseListCoverageRefusalLeavesTheOutputAloneTest::RunTest(const FString& Parameters)
{
    // "Not measured" and "measured, and empty" must never be the same value, and the whole
    // TOptional above this function exists to keep them apart. This asserts the arithmetic's own
    // half of that: a refusal returns false and does NOT touch OutFraction.
    //
    // UNABLE TO FAIL IF the sentinel were 0.0 -- which is exactly the trap, because 0.0 is what a
    // refusal that helpfully zeroed its output would leave, and the test would pass against the
    // implementation it is meant to reject. The sentinel is therefore a value the function could
    // not legally produce: fractions are in [0,1].
    const TArray<FColor> Reference = PoseListCoverageMakeBuffer(100);

    {
        const TArray<FColor> Shorter = PoseListCoverageMakeBuffer(99);
        double Fraction = 7.5;
        TestFalse(TEXT("buffers of different lengths are not comparable"),
            PinWrightPoseCapture::MeasureChangedPixelFraction(Reference, Shorter, 8, Fraction));
        TestEqual(TEXT("a length mismatch leaves the fraction untouched"), Fraction, 7.5, 0.0);
    }
    {
        const TArray<FColor> Empty;
        double Fraction = 7.5;
        TestFalse(TEXT("an empty reference is not comparable"),
            PinWrightPoseCapture::MeasureChangedPixelFraction(Empty, Reference, 8, Fraction));
        TestEqual(TEXT("an empty reference leaves the fraction untouched"), Fraction, 7.5, 0.0);

        // Both empty is still a refusal, not a vacuous 0.0: two frames that returned no pixels
        // measure nothing, and reporting 0.000 for them would say the subject is missing.
        double BothEmptyFraction = 7.5;
        TestFalse(TEXT("two empty buffers are not comparable"),
            PinWrightPoseCapture::MeasureChangedPixelFraction(Empty, Empty, 8, BothEmptyFraction));
        TestEqual(TEXT("two empty buffers leave the fraction untouched"),
            BothEmptyFraction, 7.5, 0.0);
    }

    // PROOF THE SENTINEL IS ACTUALLY OVERWRITABLE. Without this, every assertion above would also
    // pass against a function that never writes OutFraction under any circumstances.
    double WritableFraction = 7.5;
    if (TestTrue(TEXT("a comparable pair still succeeds"),
            PinWrightPoseCapture::MeasureChangedPixelFraction(Reference, Reference, 8,
                WritableFraction)))
    {
        TestEqual(TEXT("a successful measurement overwrites the sentinel"),
            WritableFraction, 0.0, 0.0);
    }
    return true;
}

// ---- the sequence RunPoseListCapture runs around it ----

namespace
{
    // The injected frame-drawing step, returning pixels that depend on whether the stub's subject
    // is currently visible. That dependency is the point: it is what makes the differential below a
    // test of the SEQUENCE (hide, draw, show, draw, diff) rather than of the arithmetic again.
    struct FPoseListCoverageStubViewport
    {
        struct FDrawnFrame
        {
            FString Filename;
            bool bWarmupFrame = false;
            bool bRetainPixelsRequested = false;
            // What the stub's subject visibility was at the moment this frame was drawn.
            bool bSubjectVisible = true;
        };

        TArray<FDrawnFrame> Frames;
        // Every value the visibility setter was asked for, in order. Two per measured shot: false
        // to draw the reference, true to put the subject back.
        TArray<bool> VisibilityRequests;
        bool bSubjectVisible = true;
        int32 VisibilityCalls = 0;
        int32 FailVisibilityAtCall = INDEX_NONE;

        // The frame is PixelCount pixels of backdrop; when the subject is visible, SubjectPixels of
        // them are painted by the subject instead. SubjectPixels = 0 is a subject that contributes
        // nothing to the picture -- an effect with no particles alive at this instant, which is the
        // exact case this whole seam exists to catch.
        int32 PixelCount = 400;
        int32 SubjectPixels = 100;

        // Path reported for throwaway frames. Empty keeps the test off the filesystem entirely.
        FString ThrowawayPath;

        PinWrightPoseCapture::FPoseListCaptureRequest::FSubjectVisibilitySetter MakeVisibilitySetter()
        {
            return [this](bool bVisible, FString& OutErrCode, FString& OutErrMsg) -> bool
            {
                const int32 CallIndex = VisibilityCalls++;
                VisibilityRequests.Add(bVisible);
                if (CallIndex == FailVisibilityAtCall)
                {
                    OutErrCode = ErrorCodes::ERR_CAPTURE_FAILED;
                    OutErrMsg = TEXT("stub visibility setter refused");
                    return false;
                }
                bSubjectVisible = bVisible;
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
                FDrawnFrame Drawn;
                Drawn.Filename = Frame.Filename;
                Drawn.bWarmupFrame = bWarmupFrame;
                Drawn.bRetainPixelsRequested = Frame.bRetainPixels;
                Drawn.bSubjectVisible = bSubjectVisible;
                Frames.Add(Drawn);

                OutCapture.Filename = Frame.Filename;
                OutCapture.Width = Frame.Width;
                OutCapture.Height = Frame.Height;
                OutCapture.Path = bWarmupFrame
                    ? ThrowawayPath
                    : FString::Printf(TEXT("/stub/%s"), *Frame.Filename);
                OutCapture.EffectiveLocation = Frame.Location;
                OutCapture.EffectiveRotation = Frame.Rotation;

                // MIRRORS THE REAL READBACK: pixels are retained only when the request asked for
                // them (PreviewViewportCaptureUtils.cpp, inside ReadViewport). So a primitive that
                // forgot to set bRetainPixels produces empty buffers here and every coverage
                // assertion below goes unset -- the plumbing is under test, not assumed.
                if (Frame.bRetainPixels)
                {
                    OutCapture.Pixels.Init(FColor(64, 64, 64, 255), PixelCount);
                    if (bSubjectVisible)
                    {
                        const int32 Painted = FMath::Min(SubjectPixels, PixelCount);
                        for (int32 Index = 0; Index < Painted; ++Index)
                        {
                            OutCapture.Pixels[Index] = FColor(240, 200, 64, 255);
                        }
                    }
                }
                return true;
            };
        }
    };

    PinWrightPoseCapture::FPoseListCaptureRequest PoseListCoverageRequest(int32 PoseCount)
    {
        PinWrightPoseCapture::FPoseListCaptureRequest Request;
        Request.FilenamePrefix = TEXT("PoseListCoverageTest");
        Request.Subdirectory = TEXT("PoseListCoverageTest");
        Request.bMeasureSubjectCoverage = true;
        for (int32 Index = 0; Index < PoseCount; ++Index)
        {
            PinWrightPoseCapture::FCameraPose Pose;
            Pose.Location = FVector(-1000.0, 0.0, 0.0);
            Pose.Rotation = FRotator(0.0, 0.0, 0.0);
            Pose.Filename = FString::Printf(TEXT("PoseListCoverageTest_shot%02d.png"), Index);
            Request.Poses.Add(MoveTemp(Pose));
        }
        return Request;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPoseListCoverageDrawsAReferencePerShotTest,
    "PinWright.render.pose_list.CoverageDrawsOneHiddenReferenceFramePerShot",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPoseListCoverageDrawsAReferencePerShotTest::RunTest(const FString& Parameters)
{
    PinWrightPoseCapture::FPoseListCaptureRequest Request = PoseListCoverageRequest(3);
    Request.bWarmupShot = true;

    FPoseListCoverageStubViewport Stub;
    Request.SubjectVisibilitySetter = Stub.MakeVisibilitySetter();

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

    TestTrue(TEXT("the set reports that coverage was requested"), Output.bCoverageRequested);
    TestEqual(TEXT("three shots were captured"), Output.Captures.Num(), 3);
    // PRECONDITION, not decoration: the frame total below counts the warm-up, so a set whose
    // warm-up never ran would make that total read correct for the wrong reason.
    TestTrue(TEXT("the warm-up shot was taken"), Output.bWarmupShotTaken);
    TestEqual(TEXT("frames drawn: warm-up, then a reference and a real frame per shot"),
        Stub.Frames.Num(), 1 + 3 * 2);
    TestEqual(TEXT("one reference frame per shot"), Output.CoverageReferenceShots, 3);

    // THE SUBJECT WAS ACTUALLY HIDDEN FOR THE REFERENCE AND SHOWN FOR THE SHOT. Without this the
    // measured 0.25 below could come from two identical draws that merely differed in filename.
    //
    // UNABLE TO FAIL IF the stub ignored the setter: Stub.bSubjectVisible is written only by the
    // setter and read only by the capturer, so these two flags are the setter's effect and nothing
    // else.
    for (int32 ShotIndex = 0; ShotIndex < 3; ++ShotIndex)
    {
        const int32 ReferenceFrame = 1 + ShotIndex * 2;
        const int32 RealFrame = ReferenceFrame + 1;
        TestFalse(*FString::Printf(TEXT("shot %d's reference was drawn with the subject hidden"),
            ShotIndex), Stub.Frames[ReferenceFrame].bSubjectVisible);
        TestTrue(*FString::Printf(TEXT("shot %d's reference is flagged as a throwaway frame"),
            ShotIndex), Stub.Frames[ReferenceFrame].bWarmupFrame);
        TestTrue(*FString::Printf(TEXT("shot %d itself was drawn with the subject shown"),
            ShotIndex), Stub.Frames[RealFrame].bSubjectVisible);
        TestFalse(*FString::Printf(TEXT("shot %d itself is not a throwaway frame"), ShotIndex),
            Stub.Frames[RealFrame].bWarmupFrame);
        TestTrue(*FString::Printf(TEXT("shot %d retained its pixels"), ShotIndex),
            Stub.Frames[RealFrame].bRetainPixelsRequested);
    }

    // THE SUBJECT IS LEFT VISIBLE. A set that ended with the subject hidden would leave the artist
    // an empty preview tab and every later capture in the session measuring nothing.
    TestTrue(TEXT("the subject is visible when the set ends"), Stub.bSubjectVisible);
    TestEqual(TEXT("the visibility setter ran twice per shot and no more"),
        Stub.VisibilityCalls, 6);

    // THE NUMBER. 100 subject pixels in 400 is 0.25 -- not 0, not 1, and not the shot count, so it
    // cannot be produced by a stuck value or by a count of the wrong thing.
    for (int32 ShotIndex = 0; ShotIndex < 3; ++ShotIndex)
    {
        const TOptional<double> Coverage =
            PinWrightPoseCapture::GetPoseSubjectCoverage(Output, ShotIndex);
        if (TestTrue(*FString::Printf(TEXT("shot %d carries a coverage figure"), ShotIndex),
                Coverage.IsSet()))
        {
            TestEqual(*FString::Printf(TEXT("shot %d's subject covers a quarter of the frame"),
                ShotIndex), Coverage.GetValue(), 0.25, 0.0);
        }
    }

    // Out of range is UNSET, never 0.0 -- the same distinction the optional exists for.
    TestFalse(TEXT("a shot index past the end carries no figure"),
        PinWrightPoseCapture::GetPoseSubjectCoverage(Output, 3).IsSet());
    TestFalse(TEXT("a negative shot index carries no figure"),
        PinWrightPoseCapture::GetPoseSubjectCoverage(Output, -1).IsSet());

    // The published shape.
    TSharedPtr<FJsonObject> ShotObj = MakeShared<FJsonObject>();
    PinWrightPoseCapture::AddPoseCoverageField(Output, 0, Request.CoverageWarnFraction, ShotObj);
    if (TestTrue(TEXT("the shot publishes subjectCoverage"),
            ShotObj->HasField(TEXT("subjectCoverage"))))
    {
        TestEqual(TEXT("the published number is the measured one"),
            ShotObj->GetNumberField(TEXT("subjectCoverage")), 0.25, 0.0);
    }
    TestFalse(TEXT("a well-covered shot carries no coverageWarning"),
        ShotObj->HasField(TEXT("coverageWarning")));

    const TSharedPtr<FJsonObject> PoseSet = PinWrightPoseCapture::MakePoseSetInfoObject(Output);
    if (TestTrue(TEXT("poseSet reports coverageReferenceShots"),
            PoseSet->HasField(TEXT("coverageReferenceShots"))))
    {
        TestEqual(TEXT("poseSet.coverageReferenceShots"),
            static_cast<int32>(PoseSet->GetNumberField(TEXT("coverageReferenceShots"))), 3);
    }
    TestFalse(TEXT("a fully measured set carries no set-level coverageWarning"),
        PoseSet->HasField(TEXT("coverageWarning")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPoseListCoverageEmptyFrameScoresZeroTest,
    "PinWright.render.pose_list.CoverageScoresZeroOnAFrameWithNoSubjectInIt",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPoseListCoverageEmptyFrameScoresZeroTest::RunTest(const FString& Parameters)
{
    // THE CASE THIS WHOLE SEAM EXISTS FOR, in unit form. A subject that paints no pixels when it is
    // shown is a Niagara system with nothing alive at the captured instant: the two draws come back
    // identical, and the honest reading of that picture is that it contains no subject.
    //
    // The real frame this reproduces is ring_t0500ms_az120_el20.png, which reported
    // boundsInFrame:true, blank:false and litPixelFraction:1.0 over an empty picture. A GEOMETRIC
    // metric scores it ~0.58 (see the file header for the arithmetic); the differential scores it
    // 0.000 and warns.
    //
    // A ZERO IS THE EASIEST NUMBER IN THE WORLD TO PRODUCE BY ACCIDENT, so this test measures BOTH
    // ends on the same code path with one variable changed between them -- SubjectPixels. If the
    // covered run below did not return 0.25, the 0.0 above it would be evidence of nothing.
    double CoveredValue = -1.0;
    {
        PinWrightPoseCapture::FPoseListCaptureRequest Request = PoseListCoverageRequest(1);
        Request.bWarmupShot = false;
        FPoseListCoverageStubViewport Stub;
        Stub.SubjectPixels = 100;
        Request.SubjectVisibilitySetter = Stub.MakeVisibilitySetter();

        PinWrightPoseCapture::FPoseListCaptureOutput Output;
        FString ErrorCode;
        FString ErrorMessage;
        if (!TestTrue(TEXT("the covered control set captured"),
                PinWrightPoseCapture::RunPoseListCapture(Request, Stub.MakeCapturer(), Output,
                    ErrorCode, ErrorMessage)))
        {
            AddError(FString::Printf(TEXT("control run failed: %s / %s"), *ErrorCode, *ErrorMessage));
            return false;
        }
        const TOptional<double> Coverage =
            PinWrightPoseCapture::GetPoseSubjectCoverage(Output, 0);
        if (!TestTrue(TEXT("the control shot carries a coverage figure"), Coverage.IsSet()))
        {
            return false;
        }
        CoveredValue = Coverage.GetValue();
        TestEqual(TEXT("the control shot measures a quarter of the frame"), CoveredValue, 0.25, 0.0);

        TSharedPtr<FJsonObject> ControlObj = MakeShared<FJsonObject>();
        PinWrightPoseCapture::AddPoseCoverageField(Output, 0, Request.CoverageWarnFraction,
            ControlObj);
        TestFalse(TEXT("the control shot carries no coverageWarning"),
            ControlObj->HasField(TEXT("coverageWarning")));
    }

    // The same set, the same stub, the same call -- with the subject painting nothing.
    PinWrightPoseCapture::FPoseListCaptureRequest Request = PoseListCoverageRequest(1);
    Request.bWarmupShot = false;
    FPoseListCoverageStubViewport Stub;
    Stub.SubjectPixels = 0;
    Request.SubjectVisibilitySetter = Stub.MakeVisibilitySetter();

    PinWrightPoseCapture::FPoseListCaptureOutput Output;
    FString ErrorCode;
    FString ErrorMessage;
    if (!TestTrue(TEXT("the empty set still captured"), PinWrightPoseCapture::RunPoseListCapture(
            Request, Stub.MakeCapturer(), Output, ErrorCode, ErrorMessage)))
    {
        AddError(FString::Printf(TEXT("empty run failed: %s / %s"), *ErrorCode, *ErrorMessage));
        return false;
    }

    // A capture, not a refusal. An empty frame is a REPORTED result, never an error: refusing it
    // would turn a review aid into a new way for a capture to fail, and the caller would lose the
    // picture that proves the effect is missing.
    TestEqual(TEXT("the empty frame was still captured"), Output.Captures.Num(), 1);
    TestEqual(TEXT("its reference frame was still drawn"), Output.CoverageReferenceShots, 1);

    const TOptional<double> Coverage = PinWrightPoseCapture::GetPoseSubjectCoverage(Output, 0);
    // MEASURED, not absent. This is the distinction the whole TOptional carries: an empty frame is
    // a shot that WAS measured and came back 0.000, which is a completely different statement from
    // a shot nobody could measure.
    //
    // UNABLE TO FAIL IF the differential silently never ran: it would then be unset and this
    // assertion would fail rather than pass.
    if (!TestTrue(TEXT("the empty frame carries a MEASURED coverage figure"), Coverage.IsSet()))
    {
        return false;
    }
    TestEqual(TEXT("a frame containing no subject scores exactly 0.000"),
        Coverage.GetValue(), 0.0, 0.0);
    TestNotEqual(TEXT("the empty and covered runs differ, so the zero is a measurement"),
        Coverage.GetValue(), CoveredValue);

    // AND IT SAYS SO. A 0.000 nobody reads is the same as no measurement at all.
    TSharedPtr<FJsonObject> ShotObj = MakeShared<FJsonObject>();
    PinWrightPoseCapture::AddPoseCoverageField(Output, 0, Request.CoverageWarnFraction, ShotObj);
    if (TestTrue(TEXT("the empty shot publishes subjectCoverage"),
            ShotObj->HasField(TEXT("subjectCoverage"))))
    {
        TestEqual(TEXT("the published number is 0.000"),
            ShotObj->GetNumberField(TEXT("subjectCoverage")), 0.0, 0.0);
    }
    TestTrue(TEXT("the empty shot carries a coverageWarning"),
        ShotObj->HasField(TEXT("coverageWarning")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPoseListCoverageAbsentWithoutSetterTest,
    "PinWright.render.pose_list.CoverageIsAbsentWhenTheSubjectCannotBeHidden",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPoseListCoverageAbsentWithoutSetterTest::RunTest(const FString& Parameters)
{
    // A kind that cannot hide its subject cannot be measured this way, and that is NOT an error --
    // it is an absent number. Asking for coverage without a visibility setter is therefore ignored
    // rather than refused, and the set says so through bCoverageRequested rather than through a
    // zero that would read as "measured, and the subject is missing".
    PinWrightPoseCapture::FPoseListCaptureRequest Request = PoseListCoverageRequest(3);
    Request.bWarmupShot = true;
    // bMeasureSubjectCoverage is already true from the helper; no setter is bound. That asymmetry
    // IS the test.
    TestTrue(TEXT("the request does ask for coverage"), Request.bMeasureSubjectCoverage);

    FPoseListCoverageStubViewport Stub;
    PinWrightPoseCapture::FPoseListCaptureOutput Output;
    FString ErrorCode;
    FString ErrorMessage;
    if (!TestTrue(TEXT("the set captured anyway"), PinWrightPoseCapture::RunPoseListCapture(
            Request, Stub.MakeCapturer(), Output, ErrorCode, ErrorMessage)))
    {
        AddError(FString::Printf(TEXT("RunPoseListCapture failed: %s / %s"),
            *ErrorCode, *ErrorMessage));
        return false;
    }

    TestFalse(TEXT("the set reports that coverage was NOT measurable"), Output.bCoverageRequested);
    TestEqual(TEXT("no reference frames were drawn"), Output.CoverageReferenceShots, 0);
    // UNABLE TO FAIL IF the frame count were not pinned: a set that drew reference frames anyway
    // would still report 0 references if the counter were simply never incremented. Counting the
    // DRAWS is what makes this an observation rather than a restatement.
    TestEqual(TEXT("only the warm-up and the three shots were drawn"), Stub.Frames.Num(), 4);
    TestEqual(TEXT("the visibility setter was never entered"), Stub.VisibilityCalls, 0);
    // And no 4 MB buffer was held per shot for a measurement that could not run.
    TestFalse(TEXT("shot 0 did not retain its pixels"), Stub.Frames[1].bRetainPixelsRequested);

    for (int32 ShotIndex = 0; ShotIndex < 3; ++ShotIndex)
    {
        TestFalse(*FString::Printf(TEXT("shot %d carries no coverage figure"), ShotIndex),
            PinWrightPoseCapture::GetPoseSubjectCoverage(Output, ShotIndex).IsSet());
    }

    TSharedPtr<FJsonObject> ShotObj = MakeShared<FJsonObject>();
    PinWrightPoseCapture::AddPoseCoverageField(Output, 0, Request.CoverageWarnFraction, ShotObj);
    TestFalse(TEXT("an unmeasured shot publishes no subjectCoverage"),
        ShotObj->HasField(TEXT("subjectCoverage")));
    TestFalse(TEXT("an unmeasured shot publishes no coverageWarning"),
        ShotObj->HasField(TEXT("coverageWarning")));

    const TSharedPtr<FJsonObject> PoseSet = PinWrightPoseCapture::MakePoseSetInfoObject(Output);
    TestFalse(TEXT("poseSet publishes no coverageReferenceShots"),
        PoseSet->HasField(TEXT("coverageReferenceShots")));
    TestFalse(TEXT("poseSet publishes no coverageWarning"),
        PoseSet->HasField(TEXT("coverageWarning")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPoseListCoverageFailedShowStopsTheSetTest,
    "PinWright.render.pose_list.CoverageStopsTheSetIfTheSubjectCannotBeShownAgain",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPoseListCoverageFailedShowStopsTheSetTest::RunTest(const FString& Parameters)
{
    // THE ONE FATAL PATH IN THE WHOLE MEASUREMENT. If the subject is hidden and cannot be shown
    // again, every remaining shot is a capture of an invisible subject -- pure backdrop, measured
    // against an equally empty reference, reporting coverage 0.000 while blank, litPixelFraction
    // and boundsInFrame all still read healthy. That is precisely the silent-empty-frame failure
    // this seam was built to catch, so the catcher may not introduce it.
    PinWrightPoseCapture::FPoseListCaptureRequest Request = PoseListCoverageRequest(3);
    Request.bWarmupShot = false;

    FPoseListCoverageStubViewport Stub;
    // Call 0 is the hide for shot 0; call 1 is the show that follows it.
    Stub.FailVisibilityAtCall = 1;
    Request.SubjectVisibilitySetter = Stub.MakeVisibilitySetter();

    PinWrightPoseCapture::FPoseListCaptureOutput Output;
    FString ErrorCode;
    FString ErrorMessage;
    TestFalse(TEXT("the set is stopped"), PinWrightPoseCapture::RunPoseListCapture(
        Request, Stub.MakeCapturer(), Output, ErrorCode, ErrorMessage));

    // WHICH failure. A set that stopped for any other reason -- a refused capture, a bad argument --
    // would satisfy the assertion above just as well, so the discriminator is that shot 0's own
    // frame was never drawn: the sequence died between the reference and the real frame.
    TestEqual(TEXT("the failure is the visibility setter's own code"),
        ErrorCode, FString(ErrorCodes::ERR_CAPTURE_FAILED));
    TestTrue(TEXT("the message names the show that failed"),
        ErrorMessage.Contains(TEXT("shown again")));
    TestTrue(TEXT("the message carries the setter's own reason"),
        ErrorMessage.Contains(TEXT("stub visibility setter refused")));
    TestEqual(TEXT("only the hidden reference frame was drawn"), Stub.Frames.Num(), 1);
    TestTrue(TEXT("and it was drawn with the subject hidden"), !Stub.Frames[0].bSubjectVisible);
    TestEqual(TEXT("no shot was captured"), Output.Captures.Num(), 0);
    // UNABLE TO FAIL IF the loop simply never started: the reference frame above proves it reached
    // shot 0, and the visibility call count proves it attempted the restore.
    TestEqual(TEXT("the restore was attempted"), Stub.VisibilityCalls, 2);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPoseListCoverageFailedHideCostsOnlyTheNumberTest,
    "PinWright.render.pose_list.CoverageSurvivesAShotThatCannotBeMeasured",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPoseListCoverageFailedHideCostsOnlyTheNumberTest::RunTest(const FString& Parameters)
{
    // The other direction, and it must NOT be fatal: a refused hide never touched the subject, so
    // the set is still exactly the set that was asked for. It simply carries no figure for that
    // shot -- which is why the count of reference frames is published beside the shot count.
    PinWrightPoseCapture::FPoseListCaptureRequest Request = PoseListCoverageRequest(3);
    Request.bWarmupShot = false;

    FPoseListCoverageStubViewport Stub;
    // Call 0 is the hide for shot 0. Everything after it succeeds.
    Stub.FailVisibilityAtCall = 0;
    Request.SubjectVisibilitySetter = Stub.MakeVisibilitySetter();

    PinWrightPoseCapture::FPoseListCaptureOutput Output;
    FString ErrorCode;
    FString ErrorMessage;
    if (!TestTrue(TEXT("the set still captured"), PinWrightPoseCapture::RunPoseListCapture(
            Request, Stub.MakeCapturer(), Output, ErrorCode, ErrorMessage)))
    {
        AddError(FString::Printf(TEXT("RunPoseListCapture failed: %s / %s"),
            *ErrorCode, *ErrorMessage));
        return false;
    }

    TestEqual(TEXT("all three shots were captured"), Output.Captures.Num(), 3);
    TestEqual(TEXT("two of the three shots got a reference frame"),
        Output.CoverageReferenceShots, 2);
    // shot 0: one real frame. shots 1 and 2: a reference plus a real frame each.
    TestEqual(TEXT("the unmeasurable shot drew only its own frame"), Stub.Frames.Num(), 5);

    // THE PARALLEL ARRAY DOES NOT SHIFT. This is the assertion that catches the tempting bug of
    // appending only measured entries: shots 1 and 2 would then hold indices 0 and 1, and every
    // published number would land on the wrong picture while every count still looked right.
    TestEqual(TEXT("the coverage array is parallel to the captures"),
        Output.SubjectCoverage.Num(), Output.Captures.Num());
    TestFalse(TEXT("shot 0 carries no coverage figure"),
        PinWrightPoseCapture::GetPoseSubjectCoverage(Output, 0).IsSet());
    for (int32 ShotIndex = 1; ShotIndex < 3; ++ShotIndex)
    {
        const TOptional<double> Coverage =
            PinWrightPoseCapture::GetPoseSubjectCoverage(Output, ShotIndex);
        if (TestTrue(*FString::Printf(TEXT("shot %d carries a coverage figure"), ShotIndex),
                Coverage.IsSet()))
        {
            TestEqual(*FString::Printf(TEXT("shot %d measured its own frame"), ShotIndex),
                Coverage.GetValue(), 0.25, 0.0);
        }
    }

    // And the set says the measurement was incomplete rather than letting three shots with two
    // numbers pass as a full set.
    const TSharedPtr<FJsonObject> PoseSet = PinWrightPoseCapture::MakePoseSetInfoObject(Output);
    if (TestTrue(TEXT("poseSet reports coverageReferenceShots"),
            PoseSet->HasField(TEXT("coverageReferenceShots"))))
    {
        TestEqual(TEXT("poseSet.coverageReferenceShots"),
            static_cast<int32>(PoseSet->GetNumberField(TEXT("coverageReferenceShots"))), 2);
    }
    TestTrue(TEXT("poseSet warns that some shots carry no figure"),
        PoseSet->HasField(TEXT("coverageWarning")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPoseListCoverageReferenceFileIsDeletedTest,
    "PinWright.render.pose_list.CoverageReferenceFramesLeaveNoFilesBehind",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPoseListCoverageReferenceFileIsDeletedTest::RunTest(const FString& Parameters)
{
    // The reference frames are throwaways, and a caller who asked for three shots and found six
    // PNGs in the output directory has no way to tell which three are theirs. They are deleted the
    // same way the warm-up frame's file is.
    //
    // The one test here that touches the filesystem, because the deletion cannot be observed any
    // other way. The warm-up is turned OFF so the only file in play is the reference frame's.
    const FString ReferencePath = FPaths::Combine(FPaths::AutomationTransientDir(),
        TEXT("PoseListCoverageTest_reference.png"));
    IFileManager::Get().Delete(*ReferencePath, false, true, true);
    if (!TestTrue(TEXT("the stand-in reference file was created"),
            FFileHelper::SaveStringToFile(TEXT("not a real png"), *ReferencePath)))
    {
        return false;
    }
    // PRECONDITION. UNABLE TO FAIL IF the file had never existed: the deletion assertion below
    // would then pass against a primitive that deletes nothing at all.
    if (!TestTrue(TEXT("the file is on disk before the set runs"),
            IFileManager::Get().FileExists(*ReferencePath)))
    {
        return false;
    }

    PinWrightPoseCapture::FPoseListCaptureRequest Request = PoseListCoverageRequest(1);
    Request.bWarmupShot = false;

    FPoseListCoverageStubViewport Stub;
    Stub.ThrowawayPath = ReferencePath;
    Request.SubjectVisibilitySetter = Stub.MakeVisibilitySetter();

    PinWrightPoseCapture::FPoseListCaptureOutput Output;
    FString ErrorCode;
    FString ErrorMessage;
    const bool bCaptured = PinWrightPoseCapture::RunPoseListCapture(
        Request, Stub.MakeCapturer(), Output, ErrorCode, ErrorMessage);
    // Cleaned up before the assertions, so a failing run cannot leave the file behind for the next.
    const bool bStillOnDisk = IFileManager::Get().FileExists(*ReferencePath);
    IFileManager::Get().Delete(*ReferencePath, false, true, true);

    if (!TestTrue(TEXT("the set captured"), bCaptured))
    {
        AddError(FString::Printf(TEXT("RunPoseListCapture failed: %s / %s"),
            *ErrorCode, *ErrorMessage));
        return false;
    }
    TestEqual(TEXT("the reference frame was drawn"), Output.CoverageReferenceShots, 1);
    TestFalse(TEXT("the reference frame's file was deleted"), bStillOnDisk);

    // AND IT IS NOT ONE OF THE SHOTS. The file being gone is only half of "throwaway"; the other
    // half is that it never appears in the reported set.
    TestEqual(TEXT("one shot was reported"), Output.Captures.Num(), 1);
    TestEqual(TEXT("the reported shot is the caller's own filename"),
        Output.Captures[0].Filename, Request.Poses[0].Filename);
    return true;
}

// ---- the caller's opt-out ----

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPoseListCoverageOptOutSkipsTheDrawTest,
    "PinWright.render.pose_list.CoverageOptOutSkipsTheExtraDrawEntirely",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPoseListCoverageOptOutSkipsTheDrawTest::RunTest(const FString& Parameters)
{
    // The differential costs one extra draw + readback per shot and holds two full-size buffers at
    // a time, so measureCoverage:false has to buy something REAL: not a suppressed number over work
    // that still happened, but the work not happening. Asserted by counting the draws the stub was
    // asked for, which is the only evidence that distinguishes the two.
    //
    // The setter is BOUND here. That asymmetry is the whole point: this is the measurable case, so
    // the absence below can only come from the flag. The sibling
    // CoverageIsAbsentWhenTheSubjectCannotBeHidden covers the opposite pairing.
    FPoseListCoverageStubViewport OptOutStub;
    PinWrightPoseCapture::FPoseListCaptureRequest OptOut = PoseListCoverageRequest(3);
    OptOut.bWarmupShot = true;
    OptOut.bMeasureSubjectCoverage = false;
    OptOut.SubjectVisibilitySetter = OptOutStub.MakeVisibilitySetter();

    PinWrightPoseCapture::FPoseListCaptureOutput OptOutResult;
    FString ErrorCode;
    FString ErrorMessage;
    if (!TestTrue(TEXT("the opted-out set captured"), PinWrightPoseCapture::RunPoseListCapture(
            OptOut, OptOutStub.MakeCapturer(), OptOutResult, ErrorCode, ErrorMessage)))
    {
        AddError(FString::Printf(TEXT("RunPoseListCapture failed: %s / %s"),
            *ErrorCode, *ErrorMessage));
        return false;
    }

    TestEqual(TEXT("the opted-out set still captured every shot"), OptOutResult.Captures.Num(), 3);
    TestFalse(TEXT("the opted-out set reports no coverage request"), OptOutResult.bCoverageRequested);
    TestEqual(TEXT("no reference frames were drawn"), OptOutResult.CoverageReferenceShots, 0);
    // THE COST ASSERTION. Four frames = one warm-up + three shots. A set that drew references and
    // merely refused to publish them would report seven.
    TestEqual(TEXT("only the warm-up and the three shots were drawn"), OptOutStub.Frames.Num(), 4);
    TestEqual(TEXT("the bound visibility setter was never entered"), OptOutStub.VisibilityCalls, 0);
    TestFalse(TEXT("shot 0 held no pixel buffer for a measurement that never ran"),
        OptOutStub.Frames[1].bRetainPixelsRequested);
    for (int32 ShotIndex = 0; ShotIndex < 3; ++ShotIndex)
    {
        TestFalse(*FString::Printf(TEXT("opted out, shot %d carries no coverage figure"), ShotIndex),
            PinWrightPoseCapture::GetPoseSubjectCoverage(OptOutResult, ShotIndex).IsSet());
    }

    // ---- the same stub, the same poses, the flag the other way ----
    //
    // UNABLE TO FAIL IF only the opted-out half were asserted: every count above is also what a
    // primitive with the coverage seam ripped out returns, and every absent number is also what one
    // that cannot measure anything returns. The pairing is what makes the absence attributable to
    // the flag.
    FPoseListCoverageStubViewport OptInStub;
    PinWrightPoseCapture::FPoseListCaptureRequest OptIn = PoseListCoverageRequest(3);
    OptIn.bWarmupShot = true;
    OptIn.SubjectVisibilitySetter = OptInStub.MakeVisibilitySetter();
    TestTrue(TEXT("the opted-in request does ask for coverage"), OptIn.bMeasureSubjectCoverage);

    PinWrightPoseCapture::FPoseListCaptureOutput OptInResult;
    if (!TestTrue(TEXT("the opted-in set captured"), PinWrightPoseCapture::RunPoseListCapture(
            OptIn, OptInStub.MakeCapturer(), OptInResult, ErrorCode, ErrorMessage)))
    {
        AddError(FString::Printf(TEXT("RunPoseListCapture failed: %s / %s"),
            *ErrorCode, *ErrorMessage));
        return false;
    }

    TestTrue(TEXT("the opted-in set reports coverage was requested"), OptInResult.bCoverageRequested);
    TestEqual(TEXT("the opted-in set drew one reference per shot"),
        OptInResult.CoverageReferenceShots, 3);
    TestEqual(TEXT("the opted-in set drew three more frames than the opted-out one"),
        OptInStub.Frames.Num(), OptOutStub.Frames.Num() + 3);
    const TOptional<double> Measured = PinWrightPoseCapture::GetPoseSubjectCoverage(OptInResult, 0);
    if (TestTrue(TEXT("the opted-in set measured shot 0"), Measured.IsSet()))
    {
        // 100 subject pixels of 400: a NON-zero measured on the same code path, so the zeros above
        // are the flag's doing rather than a measurement that returns nothing whatever it is asked.
        TestEqual(TEXT("and the number is the stub's real 0.25"), Measured.GetValue(), 0.25, 1e-9);
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPoseListCoverageOptOutIsOnTheWireTest,
    "PinWright.render.pose_list.CoverageOptOutIsReachableFromTheWire",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPoseListCoverageOptOutIsOnTheWireTest::RunTest(const FString& Parameters)
{
    // A cost a caller cannot decline is not opt-out, and a default nobody can read is not a
    // contract. This asserts the parameter EXISTS on the verb that publishes coverage, that the
    // registry publishes its default rather than leaving it to prose, and that the description says
    // what turning it off costs the reader in signal.
    const FHandlerRegistration* Preview = nullptr;
    for (const FHandlerRegistration& Reg : FAutoRegisterHandler::GetPendingRegistrations())
    {
        if (Reg.MethodName == TEXT("render.capture_asset_preview"))
        {
            Preview = &Reg;
            break;
        }
    }
    if (!TestTrue(TEXT("render.capture_asset_preview is registered"), Preview != nullptr))
    {
        return false;
    }

    const FParamSpec* Spec = nullptr;
    for (const FParamSpec& Candidate : Preview->Params)
    {
        if (Candidate.Name == TEXT("measureCoverage"))
        {
            Spec = &Candidate;
            break;
        }
    }
    if (!TestTrue(TEXT("it declares measureCoverage"), Spec != nullptr))
    {
        return false;
    }

    TestEqual(TEXT("declared as a boolean"), Spec->Type, FString(TEXT("boolean")));
    TestFalse(TEXT("and optional -- a measurement nobody asked for must not become a required arg"),
        Spec->bRequired);
    // PUBLISHED, not merely stated in prose: P3 in the parity sweep is three fit-padding defaults
    // that live only in code, so a caller cannot discover any of them from the registry.
    TestEqual(TEXT("the registry publishes the default"), Spec->Default, FString(TEXT("true")));
    // The primitive defaults the other way, and the two are not in conflict: a verb that has never
    // thought about the differential must not start paying for it silently.
    const PinWrightPoseCapture::FPoseListCaptureRequest Fresh;
    TestFalse(TEXT("while the primitive itself defaults it off"), Fresh.bMeasureSubjectCoverage);

    TestTrue(TEXT("the description states the cost that justifies the knob"),
        Spec->Description.Contains(TEXT("extra draw")));
    TestTrue(TEXT("and that the number is absent rather than 0 where it cannot be measured"),
        Spec->Description.Contains(TEXT("ABSENT")));
    return true;
}
