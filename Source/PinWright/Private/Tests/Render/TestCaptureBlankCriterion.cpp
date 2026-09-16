// Copyright (c) 2026 Alexander Penkin. MIT License.

// Tests for the blank-frame criterion (PinWrightRenderCapture::CalculateCaptureImageStats).
//
// WHAT THIS DEFENDS. `blank` is not advisory: render.capture_open_level sets bRejectBlankCapture,
// so a true verdict is a hard BLANK_CAPTURE error. The shipped criterion was
//
//     bBlank = meanLuminance <= 0.01 && luminanceVariance <= 0.0001
//
// and its variance term measures the SHARE of the frame that carries content. Editor overlays --
// the world-axis gizmo, the stats text -- are drawn at a fixed pixel size, so their share falls as
// 1/pixels while a scene's share does not. Measured 2026-08-19 through the live verb, one camera
// in empty space, ev100 9, four sizes, IDENTICAL content:
//
//     256   mean 0.0016148  variance 0.00050337   lit(>0.02) 455   -> blank:false
//     512   mean 0.0004346  variance 0.00015178   lit(>0.02) 456   -> blank:false
//     1024  mean 0.0000937  variance 0.00003047   lit(>0.02) 419   -> blank:TRUE
//     2048  mean 0.0000233  variance 0.00000756   lit(>0.02) 421   -> blank:TRUE
//
// A caller who raised `width` to get more detail turned a working capture into a hard error with
// no other change. So the property under test is INVARIANCE: the same content must get the same
// verdict at every capture size, in both directions.
//
// WHY THESE FIXTURES. Each family below is a regime with a different stable statistic, taken from
// the measurements above rather than invented:
//   - a fixed-pixel-count overlay keeps its lit COUNT (455 -> 421 across a 16x change in pixels)
//     and loses its lit fraction, mean and variance;
//   - real scene content keeps its lit FRACTION (the same asset at those four sizes: 4.76%, 4.17%,
//     4.33%, 3.86%) and gains lit count.
// A criterion built on either statistic alone flips on the other regime, which is why the fixture
// set has to contain both. The OldVerdict counterfactual below is what keeps this test from being
// vacuous: it asserts the previous criterion really does flip on the overlay family, so a verdict
// that agrees at every size is evidence about the fix rather than about the fixture.
#include "Misc/AutomationTest.h"
#include "Handlers/Render/PreviewViewportCaptureUtils.h"
#include "Tests/TestUtils.h"
#include "Tests/TestSkipReporting.h"

#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Misc/Optional.h"
#include "Misc/ScopeExit.h"

namespace
{
    // Prefixed for the same reason as every other helper in Tests/Render: anonymous namespaces in
    // one Unity translation unit merge, so a bare name a sibling also uses is a latent ODR clash.

    // The capture sizes the invariance claim is made over. 64 is below the point where the lit
    // FRACTION floor takes over from the lit COUNT floor (~128k pixels) and 2048 is far above it,
    // so the set exercises both sides of that crossover as well as the 4x steps that produced the
    // shipped flip.
    const int32 BlankCriterionEdges[] = { 64, 256, 512, 1024, 2048 };

    FColor BlankCriterionGrey(double Luminance)
    {
        const int32 Channel = FMath::Clamp(FMath::RoundToInt(Luminance * 255.0), 0, 255);
        return FColor(static_cast<uint8>(Channel), static_cast<uint8>(Channel),
            static_cast<uint8>(Channel), 255);
    }

    // A frame of `Edge` x `Edge` pixels: `Background` everywhere, then `LitCount` pixels of
    // `LitLuminance` written from the start of the buffer. LitCount < 0 means "a fraction of the
    // frame" and is resolved against the pixel count by the caller.
    TArray<FColor> BlankCriterionMakeFrame(int32 Edge, double Background, int64 LitCount,
        double LitLuminance)
    {
        TArray<FColor> Pixels;
        Pixels.Init(BlankCriterionGrey(Background), Edge * Edge);
        const int64 Count = FMath::Min<int64>(LitCount, Pixels.Num());
        for (int64 Index = 0; Index < Count; ++Index)
        {
            Pixels[static_cast<int32>(Index)] = BlankCriterionGrey(LitLuminance);
        }
        return Pixels;
    }

    // The criterion this change replaced, reproduced here as the counterfactual. It exists so the
    // invariance assertions can be shown to be about the fix: a test whose fixtures never flip
    // under the OLD rule would pass just as well with the bug still in.
    bool BlankCriterionOldVerdict(const PinWrightRenderCapture::FCaptureImageStats& Stats)
    {
        return Stats.MeanLuminance <= 0.01 && Stats.LuminanceVariance <= 0.0001;
    }

    struct FBlankCriterionFixture
    {
        const TCHAR* Name;
        // Constant background luminance over the whole frame.
        double Background;
        // Lit pixels as an ABSOLUTE count (>= 0) -- the fixed-pixel-overlay regime.
        int64 AbsoluteLitPixels;
        // ...or as a FRACTION of the frame (> 0) -- the scene-content regime. Exactly one of the
        // two is used; the other is 0.
        double LitFraction;
        double LitLuminance;
        // What the criterion must answer, at every size in BlankCriterionEdges.
        bool bExpectedBlank;
        // Why this fixture exists, quoted into the failure message so a red test names the regime
        // rather than only the numbers.
        const TCHAR* Rationale;
    };
}

// ============================================================================
// The verdict does not depend on the capture size.
//
// Counterfactual: revert CalculateCaptureImageStats to `mean <= 0.01 && variance <= 0.0001` and
// the overlay fixture reports blank:false at 64/256/512 and blank:true at 1024/2048, failing the
// invariance assertion. The OldVerdictWouldHaveFlipped assertion at the end fails in the opposite
// direction if someone weakens the fixtures until they no longer discriminate.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCaptureBlankCriterionResolutionInvariantTest,
    "PinWright.render.blank_criterion.VerdictIsResolutionInvariant",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCaptureBlankCriterionResolutionInvariantTest::RunTest(const FString& Parameters)
{
    using PinWrightRenderCapture::FCaptureImageStats;

    const FBlankCriterionFixture Fixtures[] =
    {
        // --- the blank direction: these must be caught at EVERY size ---
        { TEXT("an all-black frame"), 0.0, 0, 0.0, 0.0, true,
          TEXT("nothing was drawn at all -- the case bRejectBlankCapture exists for") },
        { TEXT("a frame black but for one lit pixel"), 0.0, 1, 0.0, 1.0, true,
          TEXT("one hot pixel or encoder artefact must not buy a frame out of the blank verdict") },
        { TEXT("a frame at tonemapper-dither level"), 0.002, 0, 0.0, 0.0, true,
          TEXT("drawn over nothing: not all-zero, but carrying no content (rpc-design.md s4)") },

        // --- the not-blank direction: these must be accepted at EVERY size ---
        { TEXT("a uniform dark grey frame"), 0.015, 0, 0.0, 0.0, false,
          TEXT("a valid flat-colour or night scene has no lit pixels at all and is still a frame") },
        { TEXT("a half-white checker frame"), 0.0, 0, 0.5, 1.0, false,
          TEXT("obvious content; the regression fixture the classifier already shipped with") },
        { TEXT("a dim scene covering 4.2% of the frame"), 0.0, 0, 0.042, 0.30, false,
          TEXT("real scene content keeps its lit FRACTION as the frame grows -- measured 4.76/4.17/4.33/3.86% over 256..2048") },
        { TEXT("a black frame carrying a 456-pixel editor overlay"), 0.0, 456, 0.0, 0.56, false,
          TEXT("THE SHIPPED DEFECT: a fixed-pixel overlay keeps its lit COUNT and loses its share, so a share-based criterion flips on it") },
        { TEXT("a black frame carrying a 419-pixel editor overlay"), 0.0, 419, 0.0, 0.56, false,
          TEXT("the same overlay measured at 1024 -- the count wanders ~8% with resampling and the verdict must not") },
    };

    bool bAnyFixtureFlippedUnderOldCriterion = false;

    for (const FBlankCriterionFixture& Fixture : Fixtures)
    {
        TOptional<bool> FirstVerdict;
        TOptional<bool> FirstOldVerdict;
        bool bOldVerdictFlipped = false;

        for (const int32 Edge : BlankCriterionEdges)
        {
            const int64 PixelCount = static_cast<int64>(Edge) * static_cast<int64>(Edge);
            const int64 LitPixels = (Fixture.LitFraction > 0.0)
                ? static_cast<int64>(FMath::RoundToDouble(Fixture.LitFraction * PixelCount))
                : Fixture.AbsoluteLitPixels;

            const TArray<FColor> Pixels = BlankCriterionMakeFrame(
                Edge, Fixture.Background, LitPixels, Fixture.LitLuminance);
            const FCaptureImageStats Stats =
                PinWrightRenderCapture::CalculateCaptureImageStats(Pixels);

            // The verdict itself, at this size.
            TestEqual(FString::Printf(
                    TEXT("%s is blank:%s at %dx%d (%s)"),
                    Fixture.Name, Fixture.bExpectedBlank ? TEXT("true") : TEXT("false"),
                    Edge, Edge, Fixture.Rationale),
                Stats.bBlank, Fixture.bExpectedBlank);

            // ...and, decisively, that it is the SAME verdict as at every other size. Asserted
            // separately from the expectation above so a fixture whose expected value is itself
            // wrong still fails loudly on the invariant rather than quietly agreeing with itself.
            if (FirstVerdict.IsSet())
            {
                TestEqual(FString::Printf(
                        TEXT("%s gets the same verdict at %dx%d as at %dx%d"),
                        Fixture.Name, Edge, Edge, BlankCriterionEdges[0], BlankCriterionEdges[0]),
                    Stats.bBlank, FirstVerdict.GetValue());
            }
            else
            {
                FirstVerdict = Stats.bBlank;
            }

            // The published measurements have to agree with the verdict they are made of, or the
            // response tells the caller one thing and the boolean another. `*FString::Printf` and
            // not the FString overload: TestEqual has an int64 form only for TCHAR*, so passing an
            // FString here silently narrows the count to int32.
            const int64 ExpectedLitPixels =
                (Fixture.LitLuminance > PinWrightRenderCapture::BlankLitLuminanceThreshold)
                    ? LitPixels
                    : ((Fixture.Background > PinWrightRenderCapture::BlankLitLuminanceThreshold)
                        ? PixelCount : static_cast<int64>(0));
            TestEqual(*FString::Printf(TEXT("%s reports its lit-pixel count at %dx%d"),
                    Fixture.Name, Edge, Edge),
                Stats.LitPixelCount, ExpectedLitPixels);
            TestTrue(FString::Printf(TEXT("%s reports a lit fraction consistent with its count at %dx%d"),
                    Fixture.Name, Edge, Edge),
                FMath::IsNearlyEqual(Stats.LitPixelFraction,
                    static_cast<double>(Stats.LitPixelCount) / static_cast<double>(PixelCount),
                    1e-9));

            const bool bOld = BlankCriterionOldVerdict(Stats);
            if (FirstOldVerdict.IsSet())
            {
                bOldVerdictFlipped |= (bOld != FirstOldVerdict.GetValue());
            }
            else
            {
                FirstOldVerdict = bOld;
            }
        }

        if (bOldVerdictFlipped)
        {
            bAnyFixtureFlippedUnderOldCriterion = true;
            AddInfo(FString::Printf(
                TEXT("Counterfactual: '%s' changes verdict with capture size under the previous ")
                TEXT("mean+variance criterion, and does not under the current one."), Fixture.Name));
        }
    }

    // Non-vacuity. If no fixture would have flipped under the old rule, this test cannot tell the
    // fix from the bug and every assertion above is decoration.
    TestTrue(TEXT("at least one fixture would have flipped its verdict with capture size under the "
                  "previous mean+variance criterion, so the invariance assertions discriminate"),
        bAnyFixtureFlippedUnderOldCriterion);
    return true;
}

// ============================================================================
// An empty pixel array is blank, and says so without dividing by zero.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCaptureBlankCriterionEmptyFrameTest,
    "PinWright.render.blank_criterion.EmptyPixelArrayIsBlank",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCaptureBlankCriterionEmptyFrameTest::RunTest(const FString& Parameters)
{
    const PinWrightRenderCapture::FCaptureImageStats Stats =
        PinWrightRenderCapture::CalculateCaptureImageStats(TArray<FColor>());
    TestTrue(TEXT("an empty readback is blank"), Stats.bBlank);
    TestEqual(TEXT("an empty readback has no lit pixels"), Stats.LitPixelCount, static_cast<int64>(0));
    TestEqual(TEXT("an empty readback reports a zero lit fraction rather than a NaN"),
        Stats.LitPixelFraction, 0.0);
    return true;
}

// ============================================================================
// The live verb publishes the numbers its verdict is made of.
//
// One capture at one size: this deliberately does NOT resize between shots. Varying capture
// resolution within a burst is a known FViewport::GetHitProxy assert on this project (a burst that
// varied size crashed the editor and cost unsaved level content), and the invariance property is a
// property of a pure function, which the test above proves directly over eight content families at
// five sizes. What only a live call can show is that the shipped response carries the measurements
// and that they agree with the boolean beside them.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCaptureBlankCriterionLiveStatsPublishedTest,
    "PinWright.render.capture_open_level.BlankVerdictPublishesItsMeasurements",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCaptureBlankCriterionLiveStatsPublishedTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetNumberField(TEXT("width"), 256);
    Payload->SetNumberField(TEXT("height"), 256);
    Payload->SetStringField(TEXT("filename"), TEXT("pw_blank_criterion_probe.png"));
    // An intentionally black frame must not fail the call; the assertions below are about the
    // reported numbers, not about the picture.
    Payload->SetBoolField(TEXT("allowBlank"), true);

    FTestResponseCapture Capture;
    if (!TestTrue(TEXT("render.capture_open_level handler found"),
            InvokeHandlerWithCapture(TEXT("render.capture_open_level"), Payload, Capture)))
    {
        return false;
    }
    ON_SCOPE_EXIT
    {
        FString Path;
        if (Capture.bSuccess && Capture.Result.IsValid() &&
            Capture.Result->TryGetStringField(TEXT("path"), Path) && !Path.IsEmpty())
        {
            IFileManager::Get().Delete(*Path, false, true);
        }
    };

    if (!Capture.bSuccess)
    {
        // Headless / no level viewport. The contract from the other side: a typed diagnostic, not
        // a crash and not a success.
        const bool bTyped =
            Capture.ErrorCode == TEXT("NO_ACTIVE_LEVEL_VIEWPORT") ||
            Capture.ErrorCode == TEXT("NO_EDITOR_WORLD") ||
            Capture.ErrorCode == TEXT("EDITOR_NOT_AVAILABLE") ||
            Capture.ErrorCode == TEXT("VIEWPORT_WORLD_MISMATCH") ||
            Capture.ErrorCode == TEXT("CAPTURE_FAILED") ||
            Capture.ErrorCode == TEXT("ENCODE_FAILED") ||
            Capture.ErrorCode == TEXT("SAVE_FAILED");
        TestTrue(TEXT("an unavailable viewport fails with a typed diagnostic"), bTyped);
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-level-viewport"),
            FString::Printf(
                TEXT("Skipped the published-measurement assertions: no level viewport (%s)."),
                *Capture.ErrorCode));
        return true;
    }

    if (!TestTrue(TEXT("success result exists"), Capture.Result.IsValid()))
    {
        return false;
    }

    const TSharedPtr<FJsonObject>* ImageStats = nullptr;
    if (!TestTrue(TEXT("the capture reports imageStats"),
            Capture.Result->TryGetObjectField(TEXT("imageStats"), ImageStats) &&
            ImageStats && ImageStats->IsValid()))
    {
        return false;
    }

    double LitCount = -1.0;
    double LitFraction = -1.0;
    double Threshold = -1.0;
    const bool bHasAll =
        (*ImageStats)->TryGetNumberField(TEXT("litPixelCount"), LitCount) &&
        (*ImageStats)->TryGetNumberField(TEXT("litPixelFraction"), LitFraction) &&
        (*ImageStats)->TryGetNumberField(TEXT("litLuminanceThreshold"), Threshold);
    if (!TestTrue(TEXT("imageStats carries litPixelCount, litPixelFraction and the threshold they "
                       "were counted at"), bHasAll))
    {
        return false;
    }

    const double PixelCount = 256.0 * 256.0;
    TestTrue(TEXT("the lit fraction is the lit count over the frame"),
        FMath::IsNearlyEqual(LitFraction, LitCount / PixelCount, 1e-6));
    TestEqual(TEXT("the published threshold is the one the criterion counts at"),
        Threshold, PinWrightRenderCapture::BlankLitLuminanceThreshold);

    // The decisive one: the boolean the caller reads is derivable from the numbers beside it, so a
    // criterion that drifts away from what it publishes cannot pass.
    double Mean = 0.0;
    (*ImageStats)->TryGetNumberField(TEXT("meanLuminance"), Mean);
    const int64 LitFloor = FMath::Min<int64>(
        PinWrightRenderCapture::BlankMinLitPixels,
        FMath::Max<int64>(1, static_cast<int64>(FMath::CeilToDouble(
            PinWrightRenderCapture::BlankMinLitFraction * PixelCount))));
    const bool bExpectedBlank =
        Mean <= PinWrightRenderCapture::BlankMeanLuminance &&
        static_cast<int64>(LitCount) < LitFloor;
    TestEqual(FString::Printf(
            TEXT("the reported blank verdict follows from the reported statistics ")
            TEXT("(mean %.7f, litPixelCount %.0f, floor %lld)"),
            Mean, LitCount, static_cast<long long>(LitFloor)),
        Capture.Result->GetBoolField(TEXT("blank")), bExpectedBlank);
    // PINWRIGHT_INFO_IS_NOT_A_SKIP: the blank verdict was already asserted against these
    // numbers above; this prints them so a drift can be read off the log.
    AddInfo(FString::Printf(
        TEXT("Live 256x256 capture: mean %.7f, litPixelCount %.0f (%.6f of frame), blank %s."),
        Mean, LitCount, LitFraction,
        Capture.Result->GetBoolField(TEXT("blank")) ? TEXT("true") : TEXT("false")));
    return true;
}
