// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for B-viewport-screenshot-writes-jpeg: the shared game-viewport
// encode path (PinWrightScreenshotUtils::EncodeBitmapToPng, which
// CaptureGameViewportToPngFile calls for ui.screenshot and editor.screenshot's PIE
// branch) must emit genuine PNG bytes for a .png output path — not JPEG. Before the
// fix the encode ran FImageUtils::ThumbnailCompressImageArray, which emits JPEG for any
// image >= 8x8 (engine USE_JPEG_FOR_THUMBNAILS), so the .png files (and ui.screenshot's
// mimeType:image/png / imageBase64) carried JPEG/JFIF bytes.
//
// Headless-safe by construction: it feeds a synthetic in-code FColor bitmap straight
// into the encode helper — no GEngine->GameViewport, no PIE, no content — so it runs in
// the normal editor automation suite. The full viewport-capture path is gated behind a
// live game viewport and can only be exercised in an interactive/PIE run (see the
// sibling FUiScreenshotIncludesViewportUmgOverlayTest, which skips headless); extracting
// the encode into EncodeBitmapToPng is what makes the PNG-not-JPEG contract testable
// without a viewport. Byte-signature checks are inlined (not a shared helper) to avoid a
// unity ODR collision with the anonymous-namespace HasPngMagic in the sibling
// TestEditorViewportScreenshot.cpp.
#include "Misc/AutomationTest.h"
#include "Utils/ScreenshotUtils.h"

#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Math/Color.h"
#include "Modules/ModuleManager.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FScreenshotEncodeBitmapToPngTest,
    "PinWright.ui.screenshot.EncodesPngNotJpeg",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FScreenshotEncodeBitmapToPngTest::RunTest(const FString& Parameters)
{
    // A 16x16 opaque bitmap built in-code: >= 8x8 is the size at which the pre-fix
    // ThumbnailCompressImageArray path selected JPEG. A single solid color suffices —
    // the format of the output bytes, not their content, is under test.
    const int32 Width = 16;
    const int32 Height = 16;
    TArray<FColor> Bitmap;
    Bitmap.Init(FColor(10, 120, 220, 255), Width * Height);

    TArray<uint8> Encoded;
    const bool bEncoded =
        PinWrightScreenshotUtils::EncodeBitmapToPng(Width, Height, Bitmap, Encoded);
    TestTrue(TEXT("EncodeBitmapToPng succeeds for a valid 16x16 bitmap"), bEncoded);
    TestTrue(TEXT("encoded bytes are non-empty"), Encoded.Num() > 0);
    if (!bEncoded || Encoded.Num() < 8)
    {
        return false;
    }

    // The bytes must carry the 8-byte PNG signature 89 50 4E 47 0D 0A 1A 0A.
    static const uint8 PngSignature[8] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
    bool bHasPngSignature = true;
    for (int32 I = 0; I < 8; ++I)
    {
        if (Encoded[I] != PngSignature[I])
        {
            bHasPngSignature = false;
            break;
        }
    }
    TestTrue(TEXT("encoded bytes begin with the PNG signature (89 50 4E 47 ...)"), bHasPngSignature);

    // ... and must NOT be JPEG/JFIF (SOI marker FF D8 FF) — the exact pre-fix defect.
    const bool bIsJpeg =
        Encoded.Num() >= 3 && Encoded[0] == 0xFF && Encoded[1] == 0xD8 && Encoded[2] == 0xFF;
    TestFalse(TEXT("encoded bytes are not JPEG/JFIF (FF D8 FF)"), bIsJpeg);

    // Round-trip: a real PNG decoder accepts the bytes and recovers the dimensions,
    // proving they are a well-formed PNG rather than merely signature-prefixed.
    IImageWrapperModule& ImageWrapperModule =
        FModuleManager::LoadModuleChecked<IImageWrapperModule>("ImageWrapper");
    TSharedPtr<IImageWrapper> PngWrapper = ImageWrapperModule.CreateImageWrapper(EImageFormat::PNG);
    const bool bDecoded =
        PngWrapper.IsValid() && PngWrapper->SetCompressed(Encoded.GetData(), Encoded.Num());
    TestTrue(TEXT("encoded bytes decode as PNG"), bDecoded);
    if (bDecoded)
    {
        // IImageWrapper::GetWidth/GetHeight return int64; assign to int32 locals so
        // TestEqual binds the int32 overload unambiguously (no int32-vs-int64 conflict).
        const int32 DecodedWidth = static_cast<int32>(PngWrapper->GetWidth());
        const int32 DecodedHeight = static_cast<int32>(PngWrapper->GetHeight());
        TestEqual(TEXT("decoded PNG width matches source"), DecodedWidth, Width);
        TestEqual(TEXT("decoded PNG height matches source"), DecodedHeight, Height);
    }

    return true;
}
