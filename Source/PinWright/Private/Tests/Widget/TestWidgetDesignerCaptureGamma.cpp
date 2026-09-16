// Copyright (c) 2026 Alexander Penkin. MIT License.

// The Designer preview capture used to sRGB-encode its pixels TWICE, so every colour a
// caller sampled off a preview PNG was wrong by exactly one transfer function
// (B-screenshot-designer-double-srgb). A widget tinted with the linear value that renders as
// (37,77,103) read back (106,149,170) — which is sRGB_encode((37,77,103)/255). The image
// still decoded, still had the right shapes, and looked merely washed out, so nothing about
// the file announced that it was wrong; a caller comparing a sampled pixel against a design
// value chased a phantom, and every widget preview.png in an asset-dump mirror carried the
// same error.
//
// The two encodes came from a flag pair. FWidgetRenderer(bUseGammaCorrection=true) reaches
// FSlate3DRenderer::bGammaCorrection -> DisplayGamma != 1 -> LinearToSrgb in
// SlateElementPixelShader (encode #1), while CreateTargetFor(..., true) leaves
// bForceLinearGamma false so UTextureRenderTarget2D::IsSRGB() is true, the RHI texture gets
// TexCreate_SRGB and the ROP encodes on every write (encode #2). Nothing downstream undid it:
// ReadPixels on an 8-bit BGRA target is a byte copy and PNGCompressImageArray writes those
// bytes verbatim. The engine's own widget-to-texture consumer uses the opposite pairing
// (UWidgetComponent's bApplyGammaCorrection=false over a hardware-sRGB target); it gets away
// with FWidgetRenderer(true) elsewhere only because a double-encoded target is normally
// SAMPLED through an sRGB SRV, which decodes one of the two encodes back out. A raw ReadBack
// -> PNG bypasses that compensation entirely.
//
// WHY THE ASSERTIONS ARE SHAPED THIS WAY. The test drives
// WidgetDesignerCaptureUtil::RenderSlateWidgetToSrgbColors — the production entry point
// CapturePreviewToPng calls, and the only place in that translation unit where a widget
// becomes pixels. It constructs no renderer and no render target of its own, sets no gamma
// flag and applies no transfer function, so the only thing that can put a correctly encoded
// byte in the buffer is the shipped pairing. That is the lesson recorded at
// Tests/Assets/TestGenerateThumbnail.cpp:18-24: a test that supplies the fix it is meant to
// verify proves nothing.
//
// The expected byte is not hardcoded either — it is FLinearColor::ToFColor(/*bSRGB=*/true) of
// the tint, i.e. what any caller would compute for "this colour, encoded once". The
// double-encode reference is that same function applied a second time to the already-encoded
// value, which is precisely the defect's arithmetic.
//
// COUNTERFACTUALS.
//   * Set FWidgetRenderer(true) in RenderSlateWidgetToSrgbColors and
//     PreviewEncodesSrgbExactlyOnce fails: zero pixels match the single-encode reference and
//     every pixel matches the double-encode reference. On the ticket's tint that is
//     (106,149,170) where (37,77,103) is expected — 67-72 counts per channel against a
//     tolerance of 3.
//   * Flip BOTH flags (renderer true, target false, or renderer false and target false) and
//     it fails the other way: raw linear bytes, far darker than the single-encode reference.
//   * Delete the DrawSize guard and RenderRejectsZeroDrawSize fails — InitCustomFormat
//     check()s a positive extent, so the degenerate call kills the suite host instead of
//     returning a diagnostic.
//
// The pixel test needs a live Slate renderer and a real RHI (the suite runs without
// -NullRHI, per CLAUDE.md). On a host that cannot render it emits the
// PINWRIGHT_ASSERTIONS_SKIPPED marker rather than reporting a hollow success
// (B-test-skips-assertions-silently). RenderRejectsZeroDrawSize needs neither and runs
// everywhere.
#include "Misc/AutomationTest.h"

#include "Handlers/UI/WidgetDesignerCaptureUtil.h"
#include "Tests/TestSkipReporting.h"

#include "Brushes/SlateColorBrush.h"
#include "Framework/Application/SlateApplication.h"
#include "Math/Color.h"
#include "Misc/App.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/SNullWidget.h"

namespace
{
    // Prefixed: the plugin builds with Unity on, so file-local helpers must not collide with
    // a sibling TU's anonymous namespace.
    constexpr int32 PWWdcgDrawSize = 32;

    // Per-channel slack for the round trip. The tint is packed to 8 bits by
    // FSlateElementBatcher::PackVertexColor, decoded back to linear in the Slate vertex
    // shader and re-encoded by the render target's ROP, so a byte may land one off; the
    // engine's encoder and the RHI's differ in rounding as well. It stays far below the
    // 35-73 count separation between the correct and double-encoded references, which the
    // test asserts explicitly rather than assuming.
    constexpr int32 PWWdcgTolerance = 3;

    // Fraction of the frame that must carry the tint before the colour claim means anything.
    // A solid box covers the whole target, so this is a floor against a fixture that failed
    // to fill rather than a tuned threshold.
    constexpr double PWWdcgMinCoverage = 0.9;

    // Three tints, all in the range where the sRGB curve separates one encode from two by
    // tens of counts. Pure black and pure white are deliberately absent: both are fixed
    // points of the transfer function and would pass under either number of encodes.
    //   * The first is the ticket's live repro value, the linear colour that renders as
    //     #254D67 and read back (106,149,170) on the defect.
    //   * The second is dark enough that the piecewise segment of the curve is in play.
    //   * The neutral grey would also catch a channel swizzle, which a per-channel compare
    //     against three distinct values cannot.
    const FLinearColor PWWdcgTints[] = {
        FLinearColor(0.018501f, 0.074214f, 0.135727f),
        FLinearColor(0.05f, 0.20f, 0.40f),
        FLinearColor(0.5f, 0.5f, 0.5f),
    };

    // The byte a caller expects: the tint, sRGB-encoded exactly once.
    FColor PWWdcgSingleEncoded(const FLinearColor& Tint)
    {
        return Tint.ToFColor(/*bSRGB=*/true);
    }

    // The defect's arithmetic: take the correctly encoded value, treat it as if it were still
    // linear, and encode it again. This is what the buggy flag pair produced.
    FColor PWWdcgDoubleEncoded(const FLinearColor& Tint)
    {
        const FColor Once = PWWdcgSingleEncoded(Tint);
        return FLinearColor(Once.R / 255.0f, Once.G / 255.0f, Once.B / 255.0f, 1.0f)
            .ToFColor(/*bSRGB=*/true);
    }

    int32 PWWdcgMaxChannelDelta(const FColor& A, const FColor& B)
    {
        return FMath::Max3(
            FMath::Abs(int32(A.R) - int32(B.R)),
            FMath::Abs(int32(A.G) - int32(B.G)),
            FMath::Abs(int32(A.B) - int32(B.B)));
    }

    int32 PWWdcgCountWithin(const TArray<FColor>& Pixels, const FColor& Reference, int32 Tolerance)
    {
        int32 Count = 0;
        for (const FColor& Pixel : Pixels)
        {
            if (PWWdcgMaxChannelDelta(Pixel, Reference) <= Tolerance)
            {
                ++Count;
            }
        }
        return Count;
    }

    int32 PWWdcgCountOpaque(const TArray<FColor>& Pixels)
    {
        int32 Count = 0;
        for (const FColor& Pixel : Pixels)
        {
            if (Pixel.A == 255)
            {
                ++Count;
            }
        }
        return Count;
    }

    FString PWWdcgDescribe(const FColor& Color)
    {
        return FString::Printf(TEXT("(%d,%d,%d)"), Color.R, Color.G, Color.B);
    }
}

// ============================================================================
// The encode count, asserted on read-back bytes.
//
// Unable to fail if: the two references were indistinguishable at this tolerance (asserted
// as a PRECONDITION per tint, so a badly chosen colour reports itself instead of passing
// vacuously), or if the widget never covered the target and there were no pixels to judge
// (asserted as a second precondition on opaque coverage). Both preconditions fail loudly and
// name what went wrong, so a fixture defect can never read as a gamma pass.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWidgetDesignerPreviewSingleSrgbEncodeTest,
    "PinWright.widget.screenshot_designer.PreviewEncodesSrgbExactlyOnce",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWidgetDesignerPreviewSingleSrgbEncodeTest::RunTest(const FString& Parameters)
{
    if (!FApp::CanEverRender() || !FSlateApplication::IsInitialized())
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-slate-renderer"),
            TEXT("FWidgetRenderer needs a live Slate renderer and a real RHI to draw into a render target; this host has neither, so no pixel was produced to judge."));
        return true;
    }

    const FIntPoint DrawSize(PWWdcgDrawSize, PWWdcgDrawSize);
    const int32 TotalPixels = DrawSize.X * DrawSize.Y;

    for (const FLinearColor& Tint : PWWdcgTints)
    {
        const FColor Once = PWWdcgSingleEncoded(Tint);
        const FColor Twice = PWWdcgDoubleEncoded(Tint);

        // PRECONDITION, not a result: if one encode and two land within tolerance of each
        // other for this tint, the colour assertions below cannot tell them apart and prove
        // nothing.
        TestTrue(FString::Printf(
            TEXT("precondition: one encode %s and two encodes %s are separable at tolerance %d"),
            *PWWdcgDescribe(Once), *PWWdcgDescribe(Twice), PWWdcgTolerance),
            PWWdcgMaxChannelDelta(Once, Twice) > PWWdcgTolerance);

        // A solid colour brush: no texture, no gradient, no style lookup — the tint reaches
        // the vertex colour unmodified, so the only transform between this FLinearColor and
        // the read-back byte is the gamma pairing under test.
        const FSlateColorBrush Brush(Tint);
        TSharedRef<SWidget> Fixture = SNew(SImage).Image(&Brush);

        TArray<FColor> Pixels;
        FString RenderError;
        if (!WidgetDesignerCaptureUtil::RenderSlateWidgetToSrgbColors(
            Fixture, DrawSize, Pixels, RenderError))
        {
            if (RenderError == TEXT("RT_CREATE_FAILED") || RenderError == TEXT("READ_PIXELS_FAILED"))
            {
                PinWrightTestSkip::SkipAssertions(*this, TEXT("render-target-unavailable"),
                    FString::Printf(TEXT("RenderSlateWidgetToSrgbColors could not produce pixels on this host (%s); the encode count was not measured."),
                        *RenderError));
                return true;
            }
            AddError(FString::Printf(
                TEXT("RenderSlateWidgetToSrgbColors failed on a %dx%d solid brush: %s"),
                DrawSize.X, DrawSize.Y, *RenderError));
            return false;
        }

        TestEqual(TEXT("the read-back buffer is Width*Height pixels"), Pixels.Num(), TotalPixels);
        if (Pixels.Num() != TotalPixels)
        {
            return false;
        }

        // PRECONDITION: the widget actually painted. Without this a frame that came back
        // empty would report "0 pixels carry the double encode" and read as a pass.
        const int32 OpaquePixels = PWWdcgCountOpaque(Pixels);
        TestTrue(FString::Printf(TEXT("precondition: the brush covered the target (%d/%d opaque pixels)"),
            OpaquePixels, TotalPixels),
            double(OpaquePixels) >= PWWdcgMinCoverage * double(TotalPixels));

        const int32 MatchOnce = PWWdcgCountWithin(Pixels, Once, PWWdcgTolerance);
        const int32 MatchTwice = PWWdcgCountWithin(Pixels, Twice, PWWdcgTolerance);

        TestTrue(FString::Printf(
            TEXT("linear tint (%f,%f,%f) reads back as %s, encoded once (%d/%d pixels within %d; sample %s)"),
            Tint.R, Tint.G, Tint.B, *PWWdcgDescribe(Once), MatchOnce, TotalPixels, PWWdcgTolerance,
            *PWWdcgDescribe(Pixels[TotalPixels / 2])),
            double(MatchOnce) >= PWWdcgMinCoverage * double(TotalPixels));

        // The direct anti-regression: not one pixel may carry the transfer function twice.
        TestEqual(FString::Printf(
            TEXT("no pixel carries the double-encoded value %s (sample %s)"),
            *PWWdcgDescribe(Twice), *PWWdcgDescribe(Pixels[TotalPixels / 2])),
            MatchTwice, 0);
    }

    return true;
}

// ============================================================================
// The degenerate size is a refusal, not a crash. UTextureRenderTarget2D::InitCustomFormat
// check()s a positive extent, so a helper that forwarded a zero size would take the suite
// host down with it. Runs on every host — no renderer needed to reach the guard.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWidgetDesignerRenderRejectsZeroDrawSizeTest,
    "PinWright.widget.screenshot_designer.RenderRejectsZeroDrawSize",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWidgetDesignerRenderRejectsZeroDrawSizeTest::RunTest(const FString& Parameters)
{
    // SNullWidget rather than the SImage fixture above: the guard returns before the widget is
    // touched, and the null widget needs neither a style set nor a renderer, so this test
    // genuinely runs on every host rather than only looking as though it does.
    TSharedRef<SWidget> Fixture = SNullWidget::NullWidget;

    const FIntPoint Degenerate[] = { FIntPoint(0, 16), FIntPoint(16, 0), FIntPoint(-4, -4) };
    for (const FIntPoint& DrawSize : Degenerate)
    {
        TArray<FColor> Pixels;
        Pixels.Add(FColor::Red); // must be emptied by the refusal, not left for a caller to read
        FString RenderError;
        const bool bRendered = WidgetDesignerCaptureUtil::RenderSlateWidgetToSrgbColors(
            Fixture, DrawSize, Pixels, RenderError);

        TestFalse(FString::Printf(TEXT("a %dx%d draw size is refused"), DrawSize.X, DrawSize.Y),
            bRendered);
        TestEqual(TEXT("the refusal reuses the existing PREVIEW_ZERO_SIZE diagnostic"),
            RenderError, FString(TEXT("PREVIEW_ZERO_SIZE")));
        TestEqual(TEXT("no pixels are handed back on refusal"), Pixels.Num(), 0);
    }

    return true;
}
