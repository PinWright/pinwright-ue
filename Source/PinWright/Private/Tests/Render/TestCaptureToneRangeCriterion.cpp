// Copyright (c) 2026 Alexander Penkin. MIT License.

// Tests for the tone-range VERDICT contract (PinWrightRenderCapture::AddToneRangeVerdictFields),
// the `crushed` / `blownOut` / `rangeWarning` sibling of the `blank` criterion that
// TestCaptureBlankCriterion.cpp defends.
//
// WHAT THIS DEFENDS. The classifier is
//
//     bToneCollapsed = ToneLevelsUsed < MinUsableToneLevels   // 8
//     bCrushed       = bToneCollapsed && MeanLuminance < 0.5
//     bBlownOut      = bToneCollapsed && !bCrushed
//
// which reads "few populated luminance levels" as "this exposure is past the end of the scene's
// range". That inference holds only for a frame the renderer was asked to draw in continuous
// tone. The SAME commit that shipped it also shipped the scoped `viewMode` capture parameter, so
// wireframe, unlit and debug-visualisation frames now arrive at this classifier -- and those are
// exactly the frames that legitimately resolve two or three levels. A two-tone wireframe was
// therefore returned with `crushed: true` at full confidence: a correct frame reported as
// unreadable, on the verb an agent uses to decide whether to re-shoot.
//
// THE FIX UNDER TEST is a gate, not a new classifier: the verdict is published only for a lit
// view mode, reusing IsLitViewMode through the `bLitViewMode` the capture already measures and
// already reports as `viewport.lit`. That is the same term that makes the exposure block's
// `pinnedFrameUsable` structurally safe (it requires `bExposurePinned`, and ExposurePinGovernsFrame
// refuses to pin a non-lit frame), so the two blocks cannot disagree about when the classifier may
// speak.
//
// WHAT AN UNLIT FRAME REPORTS, and why it is not silence. `toneRangeApplicable: false` plus a
// `toneRangeNotApplicable` explanation. Omitting `crushed` would read as "measured and fine" --
// the failure geometry.audit_static_meshes' per-check tallies exist to prevent, where
// not-applicable is a real answer counted beside clean rather than folded into it
// (MeshAuditUtils.h, `Applicable + NotApplicable == assetsExamined`).
//
// THE DELIBERATE BOUNDARY. Inside a LIT view mode a two-tone frame is still flagged, and the last
// test below pins that on purpose. Two resolved levels out of 256 from a renderer that was asked
// for full shading is the exposure failure the verdict exists to name; the defect was never that
// the threshold is wrong, it was that frames drawn under a different contract were being judged
// by it.
#include "Misc/AutomationTest.h"
#include "Handlers/Render/PreviewViewportCaptureUtils.h"
#include "Tests/TestUtils.h"

#include "Dom/JsonObject.h"

namespace
{
    // Prefixed for the same reason as every other helper in Tests/Render: anonymous namespaces in
    // one Unity translation unit merge, so a bare name a sibling also uses is a latent ODR clash.

    FColor ToneRangeGrey(double Luminance)
    {
        const int32 Channel = FMath::Clamp(FMath::RoundToInt(Luminance * 255.0), 0, 255);
        return FColor(static_cast<uint8>(Channel), static_cast<uint8>(Channel),
            static_cast<uint8>(Channel), 255);
    }

    // A frame of exactly two luminances, `Share` of it at `Foreground` and the rest at
    // `Background`. This is the shape of a wireframe or a flat-shaded unlit frame: not a
    // degenerate fixture, but what those modes are asked to produce.
    TArray<FColor> ToneRangeMakeTwoToneFrame(int32 Edge, double Background, double Foreground,
        double Share)
    {
        TArray<FColor> Pixels;
        Pixels.Init(ToneRangeGrey(Background), Edge * Edge);
        const int32 Lit = FMath::Clamp(
            FMath::RoundToInt(Share * static_cast<double>(Pixels.Num())), 0, Pixels.Num());
        for (int32 Index = 0; Index < Lit; ++Index)
        {
            Pixels[Index] = ToneRangeGrey(Foreground);
        }
        return Pixels;
    }

    // A frame spread over many levels: a linear ramp across the whole 0..1 range. Stands in for an
    // ordinary well-exposed render, which resolves dozens of levels.
    TArray<FColor> ToneRangeMakeRampFrame(int32 Edge)
    {
        const int32 PixelCount = Edge * Edge;
        TArray<FColor> Pixels;
        Pixels.Reserve(PixelCount);
        for (int32 Index = 0; Index < PixelCount; ++Index)
        {
            Pixels.Add(ToneRangeGrey(
                static_cast<double>(Index) / static_cast<double>(FMath::Max(1, PixelCount - 1))));
        }
        return Pixels;
    }

    // The capture a handler would hand to AddToneRangeVerdictFields, built without a viewport:
    // measured pixels plus the two things the report is derived from, the lit-ness flag and the
    // mode's two spellings.
    PinWrightRenderCapture::FViewportCaptureOutput ToneRangeMakeCapture(
        const TArray<FColor>& Pixels, EViewModeIndex ViewMode)
    {
        PinWrightRenderCapture::FViewportCaptureOutput Capture;
        Capture.ImageStats = PinWrightRenderCapture::CalculateCaptureImageStats(Pixels);
        Capture.bLitViewMode = PinWrightRenderCapture::IsLitViewMode(ViewMode);
        Capture.ViewModeKey = PinWrightRenderCapture::GetViewModeKey(ViewMode);
        Capture.ViewMode = Capture.ViewModeKey;
        return Capture;
    }

    TSharedPtr<FJsonObject> ToneRangeVerdictFor(
        const PinWrightRenderCapture::FViewportCaptureOutput& Capture)
    {
        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        PinWrightRenderCapture::AddToneRangeVerdictFields(Capture, Result);
        return Result;
    }

    const int32 ToneRangeEdge = 64;
}

// ============================================================================
// A frame drawn in a non-lit view mode gets no tone verdict, and says so.
//
// Counterfactual: remove the lit gate from AddToneRangeVerdictFields and the wireframe and unlit
// fixtures below come back `crushed: true` / `blownOut: true`, failing the first two assertions of
// each row. The `toneLevelsUsed` assertion at the end is what keeps the fix honest in the other
// direction: the MEASUREMENT is still published, so this is a withheld verdict and not a withheld
// number.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCaptureToneRangeNotApplicableOutsideLitTest,
    "PinWright.render.tone_range.VerdictIsWithheldOutsideLitViewModes",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCaptureToneRangeNotApplicableOutsideLitTest::RunTest(const FString& Parameters)
{
    struct FUnlitFixture
    {
        EViewModeIndex ViewMode;
        double Background;
        double Foreground;
        double Share;
        const TCHAR* Rationale;
    };

    // Every fixture is genuinely two-tone and genuinely high-contrast, because that is what these
    // modes draw. The dark-mean and bright-mean rows are both present so the gate is shown to
    // suppress `crushed` and `blownOut` -- suppressing only one would leave the other firing as
    // the exclusive `!bCrushed` half of the same collapse.
    const FUnlitFixture Fixtures[] =
    {
        { VMI_BrushWireframe, 0.0, 1.0, 0.03,
          TEXT("a wireframe: bright edges on a black field, the canonical two-tone frame") },
        { VMI_Wireframe, 0.0, 1.0, 0.03,
          TEXT("the other wireframe mode, dark-mean, which the classifier would call `crushed`") },
        { VMI_Unlit, 0.85, 0.10, 0.05,
          TEXT("flat unlit albedo, bright-mean, which the classifier would call `blownOut`") },
        { VMI_LightingOnly, 0.0, 0.9, 0.4,
          TEXT("lighting-only: no materials, so no continuous-tone contract to judge") },
    };

    for (const FUnlitFixture& Fixture : Fixtures)
    {
        const TArray<FColor> Pixels = ToneRangeMakeTwoToneFrame(
            ToneRangeEdge, Fixture.Background, Fixture.Foreground, Fixture.Share);
        const PinWrightRenderCapture::FViewportCaptureOutput Capture =
            ToneRangeMakeCapture(Pixels, Fixture.ViewMode);
        const FString Mode = Capture.ViewModeKey;

        // The fixture has to actually trip the classifier, or the assertions below are vacuous --
        // a gate tested only on frames that were never going to be flagged proves nothing.
        TestTrue(FString::Printf(
                TEXT("%s: the fixture really does collapse the classifier (%s)"),
                *Mode, Fixture.Rationale),
            Capture.ImageStats.bCrushed || Capture.ImageStats.bBlownOut);

        const TSharedPtr<FJsonObject> Verdict = ToneRangeVerdictFor(Capture);

        TestFalse(FString::Printf(TEXT("%s reports no `crushed`"), *Mode),
            Verdict->HasField(TEXT("crushed")));
        TestFalse(FString::Printf(TEXT("%s reports no `blownOut`"), *Mode),
            Verdict->HasField(TEXT("blownOut")));
        TestFalse(FString::Printf(TEXT("%s reports no `rangeWarning`"), *Mode),
            Verdict->HasField(TEXT("rangeWarning")));

        // ...and not by silence. Absent fields alone would read as "measured and fine".
        bool bApplicable = true;
        TestTrue(FString::Printf(TEXT("%s states `toneRangeApplicable`"), *Mode),
            Verdict->TryGetBoolField(TEXT("toneRangeApplicable"), bApplicable));
        TestFalse(FString::Printf(TEXT("%s states `toneRangeApplicable: false`"), *Mode),
            bApplicable);

        FString Reason;
        TestTrue(FString::Printf(TEXT("%s explains why in `toneRangeNotApplicable`"), *Mode),
            Verdict->TryGetStringField(TEXT("toneRangeNotApplicable"), Reason));
        // The explanation names the mode, so a reader holding one response knows which capture
        // parameter produced the withheld verdict without correlating blocks.
        TestTrue(FString::Printf(TEXT("%s names its own mode in the explanation"), *Mode),
            Reason.Contains(Mode));
    }

    // The measurement survives the withheld verdict: `toneLevelsUsed` is still published on the
    // same frames, so a caller who wants to judge for itself still can.
    const TArray<FColor> WirePixels = ToneRangeMakeTwoToneFrame(ToneRangeEdge, 0.0, 1.0, 0.03);
    const PinWrightRenderCapture::FViewportCaptureOutput WireCapture =
        ToneRangeMakeCapture(WirePixels, VMI_Wireframe);
    TSharedPtr<FJsonObject> ImageStats = MakeShared<FJsonObject>();
    PinWrightRenderCapture::AddToneRangeStatsFields(WireCapture.ImageStats, ImageStats);
    TestTrue(TEXT("an unlit frame still publishes `imageStats.toneLevelsUsed`"),
        ImageStats->HasField(TEXT("toneLevelsUsed")));

    return true;
}

// ============================================================================
// A lit frame keeps the verdict, in both directions.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCaptureToneRangeLitFrameKeepsVerdictTest,
    "PinWright.render.tone_range.LitFrameStillGetsTheVerdict",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCaptureToneRangeLitFrameKeepsVerdictTest::RunTest(const FString& Parameters)
{
    // --- a genuinely crushed lit frame is still flagged: the gate must not disarm the check ---
    {
        const TArray<FColor> Pixels = ToneRangeMakeTwoToneFrame(ToneRangeEdge, 0.0, 0.02, 0.001);
        const PinWrightRenderCapture::FViewportCaptureOutput Capture =
            ToneRangeMakeCapture(Pixels, VMI_Lit);
        const TSharedPtr<FJsonObject> Verdict = ToneRangeVerdictFor(Capture);

        bool bApplicable = false;
        TestTrue(TEXT("a lit frame reports `toneRangeApplicable: true`"),
            Verdict->TryGetBoolField(TEXT("toneRangeApplicable"), bApplicable) && bApplicable);
        TestFalse(TEXT("a lit frame carries no not-applicable explanation"),
            Verdict->HasField(TEXT("toneRangeNotApplicable")));

        bool bCrushed = false;
        TestTrue(TEXT("a crushed lit frame reports `crushed: true`"),
            Verdict->TryGetBoolField(TEXT("crushed"), bCrushed) && bCrushed);
        bool bBlownOut = true;
        TestTrue(TEXT("a crushed lit frame reports `blownOut: false`"),
            Verdict->TryGetBoolField(TEXT("blownOut"), bBlownOut) && !bBlownOut);
        TestTrue(TEXT("a crushed lit frame carries `rangeWarning`"),
            Verdict->HasField(TEXT("rangeWarning")));
    }

    // --- an ordinary well-exposed lit frame is clean, and says so rather than staying silent ---
    {
        const TArray<FColor> Pixels = ToneRangeMakeRampFrame(ToneRangeEdge);
        const PinWrightRenderCapture::FViewportCaptureOutput Capture =
            ToneRangeMakeCapture(Pixels, VMI_Lit);
        const TSharedPtr<FJsonObject> Verdict = ToneRangeVerdictFor(Capture);

        bool bCrushed = true;
        TestTrue(TEXT("a well-exposed lit frame reports `crushed: false`"),
            Verdict->TryGetBoolField(TEXT("crushed"), bCrushed) && !bCrushed);
        bool bBlownOut = true;
        TestTrue(TEXT("a well-exposed lit frame reports `blownOut: false`"),
            Verdict->TryGetBoolField(TEXT("blownOut"), bBlownOut) && !bBlownOut);
        TestFalse(TEXT("a well-exposed lit frame carries no `rangeWarning`"),
            Verdict->HasField(TEXT("rangeWarning")));
    }

    // --- the deliberate boundary: inside a LIT mode two tones IS the exposure failure ---
    // Pinned so a later reader does not "finish the fix" by widening the gate into the classifier
    // itself. VMI_PathTracing rather than VMI_Lit, because IsLitViewMode admits both and a gate
    // that quietly covered only one of them would be a second drift.
    {
        const TArray<FColor> Pixels = ToneRangeMakeTwoToneFrame(ToneRangeEdge, 0.0, 1.0, 0.03);
        const PinWrightRenderCapture::FViewportCaptureOutput Capture =
            ToneRangeMakeCapture(Pixels, VMI_PathTracing);
        const TSharedPtr<FJsonObject> Verdict = ToneRangeVerdictFor(Capture);

        TestTrue(TEXT("a two-tone PATH TRACED frame is still judged (lit mode)"),
            Verdict->HasField(TEXT("crushed")) && Verdict->HasField(TEXT("blownOut")));
        TestTrue(TEXT("...and is flagged, because a shading renderer resolved two levels"),
            Capture.ImageStats.bCrushed || Capture.ImageStats.bBlownOut);
    }

    // --- an unmeasured frame reports nothing at all, not a not-applicable answer ---
    // `bStatsMeasured` false means the readback returned no pixels: there is no frame to call
    // applicable or inapplicable, and claiming either would be a verdict nobody reached.
    {
        PinWrightRenderCapture::FViewportCaptureOutput Capture;
        Capture.bLitViewMode = false;
        Capture.ViewModeKey = PinWrightRenderCapture::GetViewModeKey(VMI_Wireframe);
        Capture.ViewMode = Capture.ViewModeKey;
        const TSharedPtr<FJsonObject> Verdict = ToneRangeVerdictFor(Capture);

        TestTrue(TEXT("an unmeasured frame gets no tone-range fields at all"),
            Verdict->Values.Num() == 0);
    }

    return true;
}
