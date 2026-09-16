// Copyright (c) 2026 Alexander Penkin. MIT License.

// The widget-designer preview capture was the plugin's only path from read-back pixels to
// PNG bytes with no alpha stamp. FWidgetRenderer draws into a render target whose ClearColor
// is FLinearColor::Transparent, so every pixel the widget did not cover reached disk at
// alpha 0 — a PNG that reads as blank in any alpha-compositing viewer while its RGB is
// intact. That is the identical failure mode as
// B-horizontal-orthographic-views-render-no-geometry, and it cost days there: whether the
// file "worked" depended entirely on whether the consumer composited the alpha channel or
// re-encoded without one, so the same bytes looked correct to one reader and empty to the
// next. Two verbs reached the unstamped path — widget.screenshot_designer with
// target:"preview" and asset.dump's widget aspect (preview.png) — while the same verb's
// target:"window" branch (WidgetDesignerScreenshotHandler.cpp:310) already stamped, which is
// how the gap read as fixed.
//
// A widget's alpha is real content in a way a scene's is not, so the stamp is not free: the
// ruling was to stamp anyway and PUBLISH what was stamped over, as
// FCaptureInfo::AlphaZeroFraction. This changes the output bytes for existing callers, by
// decision.
//
// WHY THE ASSERTIONS ARE SHAPED THIS WAY. The test drives
// WidgetDesignerCaptureUtil::StampOpaqueAndEncodePng — the production entry point that
// CapturePreviewToPng calls and the only route from pixels to PNG bytes in that translation
// unit. It never calls PinWrightScreenshotUtils::ForceOpaqueAlpha itself and never writes an
// alpha value, so the only way an opaque PNG comes out is if the shipped code stamped it.
// That is the lesson recorded at Tests/Assets/TestGenerateThumbnail.cpp:18-24: a test that
// supplies the fix it is meant to verify proves nothing, which is exactly why the sibling
// Tests/Render/TestCaptureOpaqueAlpha.cpp keeps passing with the production call site
// deleted.
//
// COUNTERFACTUALS.
//   * Delete the ForceOpaqueAlpha call in WidgetDesignerCaptureUtil.cpp and
//     PreviewPathStampsOpaque fails — the decoded PNG comes back with 192 of its 256 pixels
//     at alpha 0 or 128.
//   * Move the fraction measurement to after the stamp and
//     AlphaZeroFractionIsMeasuredBeforeTheStamp fails — the count is 0 for every input once
//     every alpha is 0xFF.
//   * Have the stamp write RGB as well as A and RgbIsUnchangedByTheStamp fails.
//
// Headless-safe by construction: the fixture reproduces the render target's read-back layout
// in code, so no live Designer, Slate pump or GPU is needed. There is no conditional-skip
// path here — every assertion below runs on every host, which matters because a skipped
// assertion reports success having proved nothing (board ticket
// B-test-skips-assertions-silently).
#include "Misc/AutomationTest.h"

#include "Handlers/UI/WidgetDesignerCaptureUtil.h"

#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "ImageUtils.h"
#include "Math/Color.h"
#include "Modules/ModuleManager.h"

namespace
{
    // Prefixed: the plugin builds with Unity on, so file-local helpers must not collide with
    // a sibling TU's anonymous namespace.
    constexpr int32 PWWdcaWidth = 16;
    constexpr int32 PWWdcaHeight = 16;
    constexpr int32 PWWdcaPixels = PWWdcaWidth * PWWdcaHeight;

    // Three pixel classes, matching what a Designer preview actually reads back:
    //   Drawn        - a filled widget pixel, already opaque.
    //   SoftEdge     - an anti-aliased or semi-transparent brush edge, 0 < A < 255. NOT
    //                  counted by AlphaZeroFraction, which is a strict A == 0 test; including
    //                  it is what stops the fraction assertion from passing on a helper that
    //                  merely counted "not fully opaque".
    //   Transparent  - the render target's cleared background. RGB is deliberately NON-ZERO
    //                  so a stamp that clobbered colour would be caught here too, and so the
    //                  fixture is not accidentally an all-zero blank readback.
    const FColor PWWdcaDrawn(10, 120, 220, 255);
    const FColor PWWdcaSoftEdge(200, 40, 90, 128);
    const FColor PWWdcaTransparent(7, 9, 11, 0);

    // Rows 0-3 drawn, rows 4-5 soft edge, rows 6-15 transparent:
    // 64 opaque, 32 semi-transparent, 160 fully transparent.
    constexpr int32 PWWdcaExpectedAlphaZeroPixels = 160;

    FColor PWWdcaExpectedSourcePixel(int32 Index)
    {
        const int32 Row = Index / PWWdcaWidth;
        if (Row < 4)
        {
            return PWWdcaDrawn;
        }
        return (Row < 6) ? PWWdcaSoftEdge : PWWdcaTransparent;
    }

    TArray<FColor> PWWdcaMakeReadback()
    {
        TArray<FColor> Bitmap;
        Bitmap.Reserve(PWWdcaPixels);
        for (int32 Index = 0; Index < PWWdcaPixels; ++Index)
        {
            Bitmap.Add(PWWdcaExpectedSourcePixel(Index));
        }
        return Bitmap;
    }

    // Decodes real PNG bytes back to 8-bit BGRA. Alpha is the last byte of each 4-byte group
    // in both BGRA and RGBA layouts, so the alpha assertions below do not depend on which
    // channel order the wrapper hands back.
    bool PWWdcaDecodePng(const TArray<uint8>& Png, TArray64<uint8>& OutRaw, FString& OutError)
    {
        OutRaw.Reset();
        IImageWrapperModule& ImageWrapperModule =
            FModuleManager::LoadModuleChecked<IImageWrapperModule>("ImageWrapper");
        TSharedPtr<IImageWrapper> PngWrapper = ImageWrapperModule.CreateImageWrapper(EImageFormat::PNG);
        if (!PngWrapper.IsValid() || !PngWrapper->SetCompressed(Png.GetData(), Png.Num()))
        {
            OutError = TEXT("encoded preview bytes do not decode as PNG");
            return false;
        }
        if (!PngWrapper->GetRaw(ERGBFormat::BGRA, 8, OutRaw)
            || OutRaw.Num() < int64(PWWdcaPixels) * 4)
        {
            OutError = TEXT("could not read 8-bit pixels back out of the encoded PNG");
            return false;
        }
        return true;
    }

    int32 PWWdcaCountNonOpaquePixels(const TArray64<uint8>& Raw)
    {
        int32 Count = 0;
        for (int64 Offset = 3; Offset < Raw.Num(); Offset += 4)
        {
            if (Raw[Offset] != 255)
            {
                ++Count;
            }
        }
        return Count;
    }
}

// ============================================================================
// The stamp itself, asserted on decoded PNG bytes rather than on the in-memory buffer,
// because the contract that matters is the file a consumer opens.
//
// Unable to fail if: the fixture were already fully opaque (then alpha 255 everywhere proves
// nothing about the stamp), or if the decode-and-count harness could not observe alpha 0 at
// all. Both holes are closed inside this test: the pre-stamp fraction is asserted non-zero as
// an explicit PRECONDITION before the stamp is checked, and the same fixture is encoded
// WITHOUT the stamp and asserted to decode with non-opaque pixels — a live negative control
// that fails if the harness has gone blind to transparency.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWidgetDesignerPreviewStampsOpaqueTest,
    "PinWright.widget.screenshot_designer.PreviewPathStampsOpaque",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWidgetDesignerPreviewStampsOpaqueTest::RunTest(const FString& Parameters)
{
    // Negative control FIRST: the identical fixture through the engine encoder with no stamp.
    // If this comes back opaque, the assertion further down cannot distinguish a stamped
    // frame from a transparent one and every other claim in this file is void.
    {
        const TArray<FColor> Unstamped = PWWdcaMakeReadback();
        TArray64<uint8> UnstampedPng;
        FImageUtils::PNGCompressImageArray(PWWdcaWidth, PWWdcaHeight,
            TArrayView64<const FColor>(Unstamped.GetData(), Unstamped.Num()), UnstampedPng);
        if (UnstampedPng.Num() == 0)
        {
            AddError(TEXT("negative control: engine encoder produced no bytes for a valid 16x16 bitmap"));
            return false;
        }
        TArray<uint8> ControlBytes;
        ControlBytes.SetNumUninitialized(UnstampedPng.Num());
        FMemory::Memcpy(ControlBytes.GetData(), UnstampedPng.GetData(), UnstampedPng.Num());

        TArray64<uint8> ControlRaw;
        FString ControlError;
        if (!PWWdcaDecodePng(ControlBytes, ControlRaw, ControlError))
        {
            AddError(FString::Printf(TEXT("negative control: %s"), *ControlError));
            return false;
        }
        // At minimum the 160 strictly-alpha-0 pixels must survive the round trip. The floor
        // is a floor rather than the exact 192 (160 alpha-0 + 32 alpha-128) so the control
        // cannot go red over how an encoder rounds a mid-range alpha; what it must prove is
        // only that transparency is VISIBLE to this decode-and-count harness.
        TestTrue(TEXT("negative control: an unstamped encode decodes with its transparency intact"),
            PWWdcaCountNonOpaquePixels(ControlRaw) >= PWWdcaExpectedAlphaZeroPixels);
    }

    TArray<FColor> ColorData = PWWdcaMakeReadback();
    TArray<uint8> Png;
    double AlphaZeroFraction = -1.0;
    FString Error;
    if (!WidgetDesignerCaptureUtil::StampOpaqueAndEncodePng(
        PWWdcaWidth, PWWdcaHeight, ColorData, Png, AlphaZeroFraction, Error))
    {
        AddError(FString::Printf(TEXT("StampOpaqueAndEncodePng failed on a valid 16x16 readback: %s"),
            *Error));
        return false;
    }

    // PRECONDITION, not a result: the fixture must actually have had transparency, or "every
    // pixel is opaque" is satisfied by a bitmap that arrived opaque.
    TestTrue(TEXT("precondition: the fixture carried transparency before the stamp"),
        AlphaZeroFraction > 0.0);

    TArray64<uint8> Raw;
    if (!PWWdcaDecodePng(Png, Raw, Error))
    {
        AddError(Error);
        return false;
    }

    TestEqual(TEXT("decoded preview PNG has no non-opaque pixels"),
        PWWdcaCountNonOpaquePixels(Raw), 0);

    return true;
}

// ============================================================================
// The published fraction.
//
// Unable to fail if: the measurement ran after the stamp — in which case it is 0 for every
// possible input and "non-zero on this fixture" is unreachable. Asserting the EXACT count
// (160/256, not merely "> 0") closes the weaker hole where the helper returns a constant or
// a boolean-shaped 1.0: the fixture contains 32 semi-transparent pixels that a strict A == 0
// count must exclude, so a helper counting "not fully opaque" reports 192/256 and fails here.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWidgetDesignerAlphaFractionPreStampTest,
    "PinWright.widget.screenshot_designer.AlphaZeroFractionIsMeasuredBeforeTheStamp",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWidgetDesignerAlphaFractionPreStampTest::RunTest(const FString& Parameters)
{
    TArray<FColor> ColorData = PWWdcaMakeReadback();
    TArray<uint8> Png;
    double AlphaZeroFraction = -1.0;
    FString Error;
    if (!WidgetDesignerCaptureUtil::StampOpaqueAndEncodePng(
        PWWdcaWidth, PWWdcaHeight, ColorData, Png, AlphaZeroFraction, Error))
    {
        AddError(FString::Printf(TEXT("StampOpaqueAndEncodePng failed on a valid 16x16 readback: %s"),
            *Error));
        return false;
    }

    const double Expected = double(PWWdcaExpectedAlphaZeroPixels) / double(PWWdcaPixels);
    TestEqual(TEXT("alphaZeroFraction counts exactly the strictly-alpha-0 pixels, pre-stamp"),
        AlphaZeroFraction, Expected, 1e-9);

    // And the buffer it was measured from is opaque afterwards, so nothing downstream could
    // have re-derived that number from the stamped pixels.
    int32 NonOpaqueAfter = 0;
    for (const FColor& Pixel : ColorData)
    {
        if (Pixel.A != 255)
        {
            ++NonOpaqueAfter;
        }
    }
    TestEqual(TEXT("the stamped buffer is opaque, so the fraction is unrecoverable after the stamp"),
        NonOpaqueAfter, 0);

    // A fully opaque readback must report 0, not a constant.
    TArray<FColor> AllOpaque;
    AllOpaque.Init(PWWdcaDrawn, PWWdcaPixels);
    TArray<uint8> OpaquePng;
    double OpaqueFraction = -1.0;
    if (!WidgetDesignerCaptureUtil::StampOpaqueAndEncodePng(
        PWWdcaWidth, PWWdcaHeight, AllOpaque, OpaquePng, OpaqueFraction, Error))
    {
        AddError(FString::Printf(TEXT("StampOpaqueAndEncodePng failed on an opaque readback: %s"),
            *Error));
        return false;
    }
    TestEqual(TEXT("an already-opaque readback reports alphaZeroFraction 0"),
        OpaqueFraction, 0.0, 1e-9);

    return true;
}

// ============================================================================
// RGB survives the stamp. Mirrors Tests/Render/TestCaptureOpaqueAlpha.cpp:53.
//
// Unable to fail if: it compared only the alpha channel, or compared the buffer to itself.
// The in-memory comparison is against values recomputed from the fixture definition rather
// than from a copy of the post-stamp buffer, and the decoded-PNG comparison is a second,
// independent read of the same claim through the encoder.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWidgetDesignerStampLeavesRgbTest,
    "PinWright.widget.screenshot_designer.RgbIsUnchangedByTheStamp",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWidgetDesignerStampLeavesRgbTest::RunTest(const FString& Parameters)
{
    TArray<FColor> ColorData = PWWdcaMakeReadback();
    TArray<uint8> Png;
    double AlphaZeroFraction = -1.0;
    FString Error;
    if (!WidgetDesignerCaptureUtil::StampOpaqueAndEncodePng(
        PWWdcaWidth, PWWdcaHeight, ColorData, Png, AlphaZeroFraction, Error))
    {
        AddError(FString::Printf(TEXT("StampOpaqueAndEncodePng failed on a valid 16x16 readback: %s"),
            *Error));
        return false;
    }

    bool bInMemoryRgbPreserved = true;
    for (int32 Index = 0; Index < ColorData.Num(); ++Index)
    {
        const FColor Expected = PWWdcaExpectedSourcePixel(Index);
        const FColor& Actual = ColorData[Index];
        bInMemoryRgbPreserved &= (Actual.R == Expected.R && Actual.G == Expected.G
            && Actual.B == Expected.B);
    }
    TestTrue(TEXT("the stamp leaves RGB untouched in the read-back buffer"), bInMemoryRgbPreserved);

    TArray64<uint8> Raw;
    if (!PWWdcaDecodePng(Png, Raw, Error))
    {
        AddError(Error);
        return false;
    }

    // The wrapper's channel order is not contractual here (the GetRaw overload taking a
    // format is documented as unreliable when it differs from the file's own), so require the
    // colour triple to match under ONE consistent order across the whole image rather than
    // hardcoding B,G,R. Either order proves the same thing: no colour byte moved.
    bool bMatchesBgr = true;
    bool bMatchesRgb = true;
    for (int32 Index = 0; Index < PWWdcaPixels; ++Index)
    {
        const FColor Expected = PWWdcaExpectedSourcePixel(Index);
        const int64 Offset = int64(Index) * 4;
        bMatchesBgr &= (Raw[Offset] == Expected.B && Raw[Offset + 1] == Expected.G
            && Raw[Offset + 2] == Expected.R);
        bMatchesRgb &= (Raw[Offset] == Expected.R && Raw[Offset + 1] == Expected.G
            && Raw[Offset + 2] == Expected.B);
    }
    TestTrue(TEXT("decoded PNG carries the original RGB, only alpha changed"),
        bMatchesBgr || bMatchesRgb);

    return true;
}

// ============================================================================
// Degenerate inputs keep the existing ENCODE_FAILED diagnostic rather than introducing a new
// error code (the wave's decision 6: no new codes). A short buffer is the case that used to
// walk off the end of the TArrayView64 handed to the encoder.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWidgetDesignerStampRejectsShortBufferTest,
    "PinWright.widget.screenshot_designer.StampRejectsUnencodableInput",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWidgetDesignerStampRejectsShortBufferTest::RunTest(const FString& Parameters)
{
    TArray<FColor> Short;
    Short.Init(PWWdcaDrawn, PWWdcaPixels - 1);
    TArray<uint8> Png;
    double AlphaZeroFraction = -1.0;
    FString Error;
    const bool bOk = WidgetDesignerCaptureUtil::StampOpaqueAndEncodePng(
        PWWdcaWidth, PWWdcaHeight, Short, Png, AlphaZeroFraction, Error);

    TestFalse(TEXT("a buffer shorter than Width*Height is refused"), bOk);
    TestEqual(TEXT("refusal reuses the existing ENCODE_FAILED diagnostic"), Error,
        FString(TEXT("ENCODE_FAILED")));
    TestEqual(TEXT("no PNG bytes are handed back on refusal"), Png.Num(), 0);

    return true;
}
