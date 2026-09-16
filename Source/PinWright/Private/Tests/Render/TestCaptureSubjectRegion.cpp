// Copyright (c) 2026 Alexander Penkin. MIT License.

// Tests for the SUBJECT-region measurement (PinWrightSubjectRegion), the sibling of the `blank`
// and tone-range criteria that TestCaptureBlankCriterion.cpp and TestCaptureToneRangeCriterion.cpp
// defend.
//
// WHAT THIS DEFENDS. Board ticket B-capture-asset-preview-renders-foliage-black: four foliage
// assets came back from `render.capture_asset_preview` as a PURE BLACK SUBJECT inside a CORRECTLY
// LIT preview environment, and every field the response carried was true and healthy. `blank` was
// false and correct. The frame mean was normal. The tone range was fine. All three are statistics
// OF THE FRAME, and the frame is mostly backdrop -- so an automated caller that gated on the
// response rather than on the pixels passed a useless image through, which is how the defect
// shipped.
//
// THE FIRST TEST BELOW IS THE COUNTERFACTUAL, and it is written the way round that matters: it
// asserts on the SAME buffer that the pre-existing frame statistics report a healthy frame, and
// then that the subject reading names the black subject anyway. Delete the subject measurement and
// the first half still passes -- that is the point. The frame statistics were never wrong; they
// were answering a different question, and no threshold moved on them could have caught this.
//
// THE DELIBERATE BOUNDARY, pinned by the second and third tests. The verdict is a COMPARISON
// between the subject's disc and its own backdrop, never an absolute darkness threshold, because:
//
//   - a preview profile with `bShowEnvironment` off renders a black backdrop, and an absolute
//     threshold would call every asset in it a silhouette;
//   - a subject that shaded DARKLY is not a subject that failed to shade, and the ticket's whole
//     complaint is that nothing in the response could tell those two apart.
//
// Both fixtures below are built to trip an absolute threshold and are asserted NOT to trip this
// one.
#include "Misc/AutomationTest.h"
#include "Handlers/Render/PreviewViewportCaptureUtils.h"
#include "Handlers/Render/SubjectRegionStats.h"
#include "Tests/TestUtils.h"

#include "Dom/JsonObject.h"

namespace
{
    // Prefixed for the same reason as every other helper in Tests/Render: anonymous namespaces in
    // one Unity translation unit merge, so a bare name a sibling also uses is a latent ODR clash.

    const int32 SubjectRegionEdge = 128;

    FColor SubjectRegionGrey(double Luminance)
    {
        // Grey, so the Rec.709 weights sum back to exactly the requested luminance.
        const int32 Channel = FMath::Clamp(FMath::RoundToInt(Luminance * 255.0), 0, 255);
        return FColor(static_cast<uint8>(Channel), static_cast<uint8>(Channel),
            static_cast<uint8>(Channel), 255);
    }

    // A pose whose projected subject disc is EXACTLY 32 pixels of radius at the centre of a
    // 128x128 frame, so the fixtures below can paint into a region the measurement will agree on
    // without either side inheriting the other's arithmetic.
    //
    // Camera at the origin looking down +X, fov 90 (tan of the half-angle is 1 on both axes of a
    // square frame), subject centred on the view axis at distance 100. The projected pixel radius
    // is 64 * R / sqrt(D^2 - R^2), so R = D / sqrt(5) puts it at 32.
    PinWrightSubjectRegion::FSubjectRegionView SubjectRegionCentredView()
    {
        PinWrightSubjectRegion::FSubjectRegionView View;
        View.CameraLocation = FVector::ZeroVector;
        View.CameraRotation = FRotator::ZeroRotator;
        View.Width = SubjectRegionEdge;
        View.Height = SubjectRegionEdge;
        View.bOrthographic = false;
        View.Fov = 90.0;
        View.BoundsOrigin = FVector(100.0, 0.0, 0.0);
        View.BoundsRadius = 100.0 / FMath::Sqrt(5.0);
        return View;
    }

    const double SubjectRegionDiscRadiusPixels = 32.0;

    // A frame whose backdrop is a vertical gradient of mean `Backdrop` and half-range
    // `BackdropGradient`, with a vertical band of `Subject` luminance `HalfBandWidth` pixels either
    // side of centre, CLIPPED TO THE SUBJECT DISC.
    //
    // The band rather than a filled disc on purpose. A bounding SPHERE circumscribes its asset, so
    // a real subject fills only part of its own projected disc -- a tree perhaps a quarter of it --
    // and the reading is diluted by the backdrop showing through. A fixture that filled the disc
    // would prove the measurement works only in the case that was never at risk.
    //
    // The gradient is load-bearing too, and not decoration: a flat backdrop resolves ONE luminance
    // level, which the tone-range classifier correctly calls `crushed`, and the counterfactual in
    // the first test needs a frame every existing criterion reports as HEALTHY. A preview backdrop
    // is a sky gradient over a floor, so a gradient is also what the real frame looks like.
    TArray<FColor> SubjectRegionMakeBandFrame(double Backdrop, double BackdropGradient,
        double Subject, double HalfBandWidth)
    {
        TArray<FColor> Pixels;
        Pixels.SetNumUninitialized(SubjectRegionEdge * SubjectRegionEdge);
        const double Centre = static_cast<double>(SubjectRegionEdge) * 0.5;
        const double LastRow = static_cast<double>(FMath::Max(1, SubjectRegionEdge - 1));
        for (int32 Y = 0; Y < SubjectRegionEdge; ++Y)
        {
            const double Ramp = (static_cast<double>(Y) / LastRow) - 0.5;
            const FColor BackdropColor = SubjectRegionGrey(Backdrop + (2.0 * Ramp * BackdropGradient));
            const double OffsetY = (static_cast<double>(Y) + 0.5) - Centre;
            for (int32 X = 0; X < SubjectRegionEdge; ++X)
            {
                const double OffsetX = (static_cast<double>(X) + 0.5) - Centre;
                const bool bInDisc = (OffsetX * OffsetX) + (OffsetY * OffsetY) <=
                    (SubjectRegionDiscRadiusPixels * SubjectRegionDiscRadiusPixels);
                Pixels[(Y * SubjectRegionEdge) + X] =
                    (bInDisc && FMath::Abs(OffsetX) <= HalfBandWidth)
                        ? SubjectRegionGrey(Subject)
                        : BackdropColor;
            }
        }
        return Pixels;
    }

    PinWrightSubjectRegion::FSubjectRegionStats SubjectRegionMeasure(const TArray<FColor>& Pixels,
        const PinWrightSubjectRegion::FSubjectRegionView& View)
    {
        return PinWrightSubjectRegion::MeasureSubjectRegion(Pixels, View,
            PinWrightRenderCapture::BlankLitLuminanceThreshold);
    }
}

// ============================================================================
// A black subject inside a lit frame is named -- and the frame statistics that shipped the bug
// are shown, on the same pixels, to be healthy.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCaptureSubjectRegionBlackSubjectIsNamedTest,
    "PinWright.render.subject_region.BlackSubjectInALitFrameIsNamed",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCaptureSubjectRegionBlackSubjectIsNamedTest::RunTest(const FString& Parameters)
{
    // Pure black subject, well-exposed backdrop. The band is 16 pixels wide inside a 64-pixel
    // disc, so roughly a third of the subject's projected bounds is black and two thirds of it is
    // backdrop showing past the asset -- the diluted reading a real bounding sphere gives.
    const TArray<FColor> Pixels = SubjectRegionMakeBandFrame(/*Backdrop=*/0.5,
        /*BackdropGradient=*/0.15, /*Subject=*/0.0, /*HalfBandWidth=*/8.0);

    // --- the counterfactual: everything that already existed reports a healthy frame ---
    const PinWrightRenderCapture::FCaptureImageStats FrameStats =
        PinWrightRenderCapture::CalculateCaptureImageStats(Pixels);
    TestFalse(TEXT("the frame is NOT blank, and `blank` is right about that"), FrameStats.bBlank);
    TestTrue(TEXT("the frame mean is a healthy exposure, because the backdrop dominates it"),
        FrameStats.MeanLuminance > 0.4);
    TestTrue(TEXT("nearly the whole frame reads as lit"), FrameStats.LitPixelFraction > 0.9);
    TestFalse(TEXT("the tone range is fine too"), FrameStats.bCrushed || FrameStats.bBlownOut);

    // --- ...and the subject reading names the black subject on the same pixels ---
    const PinWrightSubjectRegion::FSubjectRegionStats Stats =
        SubjectRegionMeasure(Pixels, SubjectRegionCentredView());
    TestTrue(TEXT("the subject region was measured"), Stats.bMeasured);
    // RoundToInt(double) is int64 under LWC; narrow explicitly so TestEqual picks one overload.
    TestEqual(TEXT("the projected disc is centred on the frame"),
        static_cast<int32>(FMath::RoundToInt(Stats.CenterX)), SubjectRegionEdge / 2);
    TestEqual(TEXT("...on both axes"),
        static_cast<int32>(FMath::RoundToInt(Stats.CenterY)), SubjectRegionEdge / 2);
    TestTrue(TEXT("the projected disc is the 32-pixel radius the fixture painted into"),
        FMath::IsNearlyEqual(Stats.RadiusX, SubjectRegionDiscRadiusPixels, 0.5) &&
        FMath::IsNearlyEqual(Stats.RadiusY, SubjectRegionDiscRadiusPixels, 0.5));

    TestTrue(TEXT("a substantial share of the subject's bounds is unlit"),
        Stats.SubjectUnlitFraction > 0.2);
    TestTrue(TEXT("...while its backdrop is entirely lit"),
        Stats.BackdropUnlitFraction < 0.001);
    TestTrue(TEXT("the subject's mean sits well below its backdrop's"),
        Stats.SubjectMeanLuminance < Stats.BackdropMeanLuminance);
    TestTrue(TEXT("`silhouette` fires"), Stats.bSilhouette);

    // The response says so, and the warning quotes the numbers rather than the thresholds.
    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    PinWrightSubjectRegion::AddSubjectRegionFields(Stats, Result);
    const TSharedPtr<FJsonObject>* Region = nullptr;
    TestTrue(TEXT("the response carries a `subjectRegion` block"),
        Result->TryGetObjectField(TEXT("subjectRegion"), Region) && Region != nullptr);
    if (Region != nullptr)
    {
        bool bSilhouette = false;
        TestTrue(TEXT("`subjectRegion.silhouette` is published as true"),
            (*Region)->TryGetBoolField(TEXT("silhouette"), bSilhouette) && bSilhouette);
        // The threshold the fractions were counted against travels with them, so a caller can
        // reproduce the verdict without inheriting a constant.
        TestTrue(TEXT("the lit threshold the verdict used travels with it"),
            (*Region)->HasField(TEXT("litLuminanceThreshold")));
    }
    FString Warning;
    TestTrue(TEXT("a `subjectRegionWarning` is raised"),
        Result->TryGetStringField(TEXT("subjectRegionWarning"), Warning));
    // It has to send the reader at the fields that narrow the CAUSE, because the pixels cannot
    // separate a genuinely black asset from one that failed to shade.
    TestTrue(TEXT("the warning points at the preview scene's lighting readings"),
        Warning.Contains(TEXT("showEnvironment")) && Warning.Contains(TEXT("key.intensity")));

    return true;
}

// ============================================================================
// The two frames an absolute darkness threshold would get wrong.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCaptureSubjectRegionComparisonNotThresholdTest,
    "PinWright.render.subject_region.VerdictIsAComparisonNotADarknessThreshold",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCaptureSubjectRegionComparisonNotThresholdTest::RunTest(const FString& Parameters)
{
    const PinWrightSubjectRegion::FSubjectRegionView View = SubjectRegionCentredView();

    // --- a frame that is dark EVERYWHERE: the preview profile's environment is switched off ---
    //
    // Subject and backdrop both below the lit threshold. An absolute threshold on the subject's
    // disc would call this a silhouette on every asset ever captured in such a profile; the
    // comparison sees no difference and the lit-backdrop term refuses independently.
    {
        const TArray<FColor> Pixels = SubjectRegionMakeBandFrame(/*Backdrop=*/0.005,
            /*BackdropGradient=*/0.0, /*Subject=*/0.0, /*HalfBandWidth=*/8.0);
        const PinWrightSubjectRegion::FSubjectRegionStats Stats = SubjectRegionMeasure(Pixels, View);

        TestTrue(TEXT("the region is still measured on a dark frame"), Stats.bMeasured);
        // The fixture really is the one an absolute threshold would trip on, or the assertion
        // below is vacuous.
        TestTrue(TEXT("the fixture's subject really is almost entirely unlit"),
            Stats.SubjectUnlitFraction > 0.9);
        TestFalse(TEXT("...but `silhouette` does NOT fire: the backdrop is just as dark"),
            Stats.bSilhouette);

        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        PinWrightSubjectRegion::AddSubjectRegionFields(Stats, Result);
        TestFalse(TEXT("and no warning is raised"),
            Result->HasField(TEXT("subjectRegionWarning")));
    }

    // --- a subject that shaded DARKLY, which is the distinction the ticket asks for ---
    //
    // The asset is four times darker than its backdrop and plainly readable. "Rendered dark" is
    // not "rendered black", and the measurement has to say so rather than flag every asset that
    // is not bright.
    {
        const TArray<FColor> Pixels = SubjectRegionMakeBandFrame(/*Backdrop=*/0.5,
            /*BackdropGradient=*/0.15, /*Subject=*/0.12, /*HalfBandWidth=*/8.0);
        const PinWrightSubjectRegion::FSubjectRegionStats Stats = SubjectRegionMeasure(Pixels, View);

        TestTrue(TEXT("the region was measured"), Stats.bMeasured);
        TestTrue(TEXT("nothing in the subject's bounds is unlit"),
            Stats.SubjectUnlitFraction < 0.001);
        TestFalse(TEXT("`silhouette` does NOT fire on a merely dark subject"), Stats.bSilhouette);
        // ...and the numbers a caller would judge it by are still published, so "dark" remains
        // visible even though it is not flagged.
        TestTrue(TEXT("the subject still reads darker than its backdrop"),
            Stats.SubjectMeanLuminance < Stats.BackdropMeanLuminance);
        TestTrue(TEXT("and it carries a highlight, which a black subject does not"),
            Stats.SubjectMaxLuminance > 0.4);
    }

    return true;
}

// ============================================================================
// A region that cannot be measured says why, and publishes no numbers to be mistaken for one.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCaptureSubjectRegionUnmeasurableSaysWhyTest,
    "PinWright.render.subject_region.UnmeasurableRegionsSayWhyAndPublishNoNumbers",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCaptureSubjectRegionUnmeasurableSaysWhyTest::RunTest(const FString& Parameters)
{
    const TArray<FColor> Pixels = SubjectRegionMakeBandFrame(0.5, 0.15, 0.0, 8.0);

    struct FUnmeasurableFixture
    {
        const TCHAR* Name;
        PinWrightSubjectRegion::FSubjectRegionView View;
        bool bShortBuffer;
    };

    PinWrightSubjectRegion::FSubjectRegionView NoBounds = SubjectRegionCentredView();
    NoBounds.BoundsRadius = 0.0;

    // The camera pushed inside the bounding sphere: the sphere has no bounded silhouette in the
    // frame, so any ellipse reported would be invented.
    PinWrightSubjectRegion::FSubjectRegionView InsideBounds = SubjectRegionCentredView();
    InsideBounds.BoundsOrigin = FVector(10.0, 0.0, 0.0);

    const FUnmeasurableFixture Fixtures[] =
    {
        { TEXT("no bounds"), NoBounds, false },
        { TEXT("camera inside the bounds"), InsideBounds, false },
        { TEXT("short readback"), SubjectRegionCentredView(), true },
    };

    for (const FUnmeasurableFixture& Fixture : Fixtures)
    {
        TArray<FColor> Buffer = Pixels;
        if (Fixture.bShortBuffer)
        {
            Buffer.SetNum(Buffer.Num() / 2);
        }
        const PinWrightSubjectRegion::FSubjectRegionStats Stats =
            SubjectRegionMeasure(Buffer, Fixture.View);

        TestFalse(FString::Printf(TEXT("%s: the region is not measured"), Fixture.Name),
            Stats.bMeasured);
        TestFalse(FString::Printf(TEXT("%s: no silhouette verdict is reached"), Fixture.Name),
            Stats.bSilhouette);
        // Zeroes here would read as "the subject is entirely black", the loudest possible claim,
        // reached by nobody.
        TestTrue(FString::Printf(TEXT("%s: no subject pixels are claimed"), Fixture.Name),
            Stats.SubjectPixelCount == 0);

        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        PinWrightSubjectRegion::AddSubjectRegionFields(Stats, Result);
        const TSharedPtr<FJsonObject>* Region = nullptr;
        TestTrue(FString::Printf(TEXT("%s: a `subjectRegion` block is still emitted"),
                Fixture.Name),
            Result->TryGetObjectField(TEXT("subjectRegion"), Region) && Region != nullptr);
        if (Region == nullptr)
        {
            continue;
        }
        bool bMeasured = true;
        TestTrue(FString::Printf(TEXT("%s: it states `measured: false`"), Fixture.Name),
            (*Region)->TryGetBoolField(TEXT("measured"), bMeasured) && !bMeasured);
        // Stated, never silent: an absent block reads as "the subject is fine".
        FString Reason;
        TestTrue(FString::Printf(TEXT("%s: it explains why in `notMeasured`"), Fixture.Name),
            (*Region)->TryGetStringField(TEXT("notMeasured"), Reason) && !Reason.IsEmpty());
        TestFalse(FString::Printf(TEXT("%s: it publishes no subject numbers"), Fixture.Name),
            (*Region)->HasField(TEXT("subject")));
        TestFalse(FString::Printf(TEXT("%s: and no verdict"), Fixture.Name),
            (*Region)->HasField(TEXT("silhouette")));
        TestFalse(FString::Printf(TEXT("%s: and raises no warning"), Fixture.Name),
            Result->HasField(TEXT("subjectRegionWarning")));
    }

    return true;
}

// ============================================================================
// The warning names the EXPOSURE PIN as the lead cause -- and says so only when there was one.
// ============================================================================
//
// WHY THIS EXISTS. B-capture-asset-preview-renders-foliage-black burned two investigation passes
// on the preview scene's lighting, because a correctly lit backdrop beside a black subject looks
// exactly like a lighting failure. It is not: an FAdvancedPreviewScene's backdrop is the EMISSIVE
// sky sphere and renders identically with every light in the scene switched off. What the four
// failing captures actually shared was `exposure:{mode:"fixed", ev100:-0.5}` on a preview that
// resolves to EV100 -2.12 unpinned -- 1.62 stops under, which the tone curve's toe turns into a
// black subject while barely dimming a panorama.
//
// So the warning has to lead with the exposure, and it has to lead with it ONLY when the pixels
// were actually drawn fixed -- a warning that blamed a pin on an auto-exposed frame would send
// the next reader down a second dead end. Both halves are asserted.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCaptureSubjectRegionFixedExposureIsNamedTest,
    "PinWright.render.subject_region.FixedExposureIsNamedAsTheLeadCause",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCaptureSubjectRegionFixedExposureIsNamedTest::RunTest(const FString& Parameters)
{
    // The measured failure, reproduced: a black subject inside a well-exposed backdrop.
    const TArray<FColor> Pixels = SubjectRegionMakeBandFrame(/*Backdrop=*/0.5,
        /*BackdropGradient=*/0.15, /*Subject=*/0.0, /*HalfBandWidth=*/8.0);

    // --- drawn at a fixed exposure: the pin leads ---
    {
        PinWrightSubjectRegion::FSubjectRegionView View = SubjectRegionCentredView();
        View.bFixedExposure = true;
        View.Ev100 = -0.5;
        const PinWrightSubjectRegion::FSubjectRegionStats Stats = SubjectRegionMeasure(Pixels, View);
        TestTrue(TEXT("pinned: `silhouette` still fires"), Stats.bSilhouette);

        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        PinWrightSubjectRegion::AddSubjectRegionFields(Stats, Result);
        FString Warning;
        TestTrue(TEXT("pinned: a `subjectRegionWarning` is raised"),
            Result->TryGetStringField(TEXT("subjectRegionWarning"), Warning));
        // `LEAD CAUSE` rather than the phrase "fixed exposure": FString::Contains defaults to a
        // CASE-INSENSITIVE search, and the unpinned branch below says "NOT drawn at a fixed
        // exposure", which would match that phrase and make the negative assertion vacuous.
        TestTrue(TEXT("pinned: the warning leads with the exposure as the cause"),
            Warning.Contains(TEXT("LEAD CAUSE")));
        // The number, not just the fact -- a caller reading the warning alone can tell whether the
        // EV100 in force is the one they asked for.
        TestTrue(TEXT("pinned: it quotes the EV100 the pixels were drawn at"),
            Warning.Contains(TEXT("EV100 -0.50")));
        // The remedy is the one PINWRIGHT_EXPOSURE_PARAM_DESC prescribes: DERIVE the number from
        // an auto shot rather than hand-picking it. Naming the field is what makes it actionable.
        TestTrue(TEXT("pinned: it names the auto re-shoot and the field to read off it"),
            Warning.Contains(TEXT("mode:\"auto\"")) &&
            Warning.Contains(TEXT("ev100Equivalent")));
        // The lighting fields stay in the message, demoted rather than dropped -- they are still
        // the right second lead once the exposure is known good.
        TestTrue(TEXT("pinned: the preview-scene lighting fields are still offered"),
            Warning.Contains(TEXT("showEnvironment")) && Warning.Contains(TEXT("key.intensity")));

        // No duplication: the exposure numbers live in `viewport.exposure`, and a second copy
        // under `subjectRegion` would give a caller two places to read one fact.
        const TSharedPtr<FJsonObject>* Region = nullptr;
        TestTrue(TEXT("pinned: the `subjectRegion` block is still emitted"),
            Result->TryGetObjectField(TEXT("subjectRegion"), Region) && Region != nullptr);
        if (Region != nullptr)
        {
            TestFalse(TEXT("pinned: it publishes no exposure of its own"),
                (*Region)->HasField(TEXT("exposure")) || (*Region)->HasField(TEXT("ev100")));
        }
    }

    // --- auto-exposed: the pin is ruled OUT, not blamed ---
    {
        PinWrightSubjectRegion::FSubjectRegionView View = SubjectRegionCentredView();
        // bFixedExposure defaults false. Stated rather than relied on, because the whole assertion
        // below is about that default.
        View.bFixedExposure = false;
        const PinWrightSubjectRegion::FSubjectRegionStats Stats = SubjectRegionMeasure(Pixels, View);
        TestTrue(TEXT("unpinned: `silhouette` still fires on the same pixels"), Stats.bSilhouette);

        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        PinWrightSubjectRegion::AddSubjectRegionFields(Stats, Result);
        FString Warning;
        TestTrue(TEXT("unpinned: a `subjectRegionWarning` is still raised"),
            Result->TryGetStringField(TEXT("subjectRegionWarning"), Warning));
        TestFalse(TEXT("unpinned: it does NOT blame an exposure pin"),
            Warning.Contains(TEXT("LEAD CAUSE")));
        TestFalse(TEXT("unpinned: and quotes no EV100, because none governed these pixels"),
            Warning.Contains(TEXT("EV100")));
        TestTrue(TEXT("unpinned: it says so, rather than staying silent about the exposure"),
            Warning.Contains(TEXT("NOT drawn at a fixed exposure")));
        TestTrue(TEXT("unpinned: and still offers the preview-scene lighting fields"),
            Warning.Contains(TEXT("showEnvironment")) && Warning.Contains(TEXT("key.intensity")));
    }

    return true;
}
