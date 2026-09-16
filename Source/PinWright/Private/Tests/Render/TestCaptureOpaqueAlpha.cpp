// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for B-horizontal-orthographic-views-render-no-geometry: every capture
// PinWright writes must be an OPAQUE PNG. The editor back buffer carries alpha 0 over
// scene pixels and alpha 255 only where Slate composited an editor overlay (the
// world-axis gizmo, the ortho scale bar), so encoding the raw read-back alpha produced a
// 99.97%-transparent PNG. Consumers that composite alpha saw a blank frame with the gizmo
// and scale bar alone on their own background, while consumers that re-encoded without an
// alpha channel saw the identical file render correctly — which is why the defect looked
// like "horizontal orthographic views render no geometry" while the larger perspective and
// top-down shots of the same run looked fine.
//
// Headless-safe by construction: the synthetic bitmap below reproduces the back buffer's
// alpha layout in-code, so the contract is testable without a live viewport (the capture
// itself needs an interactive editor). This is exactly why the alpha stamp lives in
// PinWrightScreenshotUtils::ForceOpaqueAlpha rather than inline in each capture path.
#include "Misc/AutomationTest.h"
#include "Utils/ScreenshotUtils.h"

#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Math/Color.h"
#include "Modules/ModuleManager.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRenderCaptureOpaqueAlphaTest,
    "PinWright.render.capture.ForcesOpaqueAlpha",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FRenderCaptureOpaqueAlphaTest::RunTest(const FString& Parameters)
{
    const int32 Width = 16;
    const int32 Height = 16;

    // Back-buffer layout: scene pixels transparent, one overlay pixel already opaque.
    const FColor ScenePixel(10, 120, 220, 0);
    const FColor OverlayPixel(255, 64, 0, 255);
    TArray<FColor> Bitmap;
    Bitmap.Init(ScenePixel, Width * Height);
    Bitmap[0] = OverlayPixel;

    PinWrightScreenshotUtils::ForceOpaqueAlpha(Bitmap);

    bool bAllOpaque = true;
    bool bColorPreserved = true;
    for (int32 Index = 0; Index < Bitmap.Num(); ++Index)
    {
        const FColor& Pixel = Bitmap[Index];
        bAllOpaque &= (Pixel.A == 255);
        const FColor& Expected = (Index == 0) ? OverlayPixel : ScenePixel;
        bColorPreserved &= (Pixel.R == Expected.R && Pixel.G == Expected.G && Pixel.B == Expected.B);
    }
    TestTrue(TEXT("every pixel is opaque after ForceOpaqueAlpha"), bAllOpaque);
    TestTrue(TEXT("ForceOpaqueAlpha leaves RGB untouched"), bColorPreserved);

    // The contract that matters is the file a consumer opens, so assert on decoded PNG
    // bytes rather than only on the in-memory bitmap.
    TArray<uint8> Encoded;
    if (!PinWrightScreenshotUtils::EncodeBitmapToPng(Width, Height, Bitmap, Encoded))
    {
        AddError(TEXT("EncodeBitmapToPng failed for a valid 16x16 bitmap"));
        return false;
    }

    IImageWrapperModule& ImageWrapperModule =
        FModuleManager::LoadModuleChecked<IImageWrapperModule>("ImageWrapper");
    TSharedPtr<IImageWrapper> PngWrapper = ImageWrapperModule.CreateImageWrapper(EImageFormat::PNG);
    if (!PngWrapper.IsValid() || !PngWrapper->SetCompressed(Encoded.GetData(), Encoded.Num()))
    {
        AddError(TEXT("encoded capture bytes do not decode as PNG"));
        return false;
    }

    TArray64<uint8> Raw;
    if (!PngWrapper->GetRaw(ERGBFormat::BGRA, 8, Raw) || Raw.Num() < Width * Height * 4)
    {
        AddError(TEXT("could not read BGRA pixels back out of the encoded PNG"));
        return false;
    }

    // FColor/BGRA byte order puts alpha last in each 4-byte group.
    int32 TransparentPixels = 0;
    for (int64 Offset = 3; Offset < Raw.Num(); Offset += 4)
    {
        if (Raw[Offset] != 255)
        {
            ++TransparentPixels;
        }
    }
    TestEqual(TEXT("decoded PNG has no non-opaque pixels"), TransparentPixels, 0);

    return true;
}
