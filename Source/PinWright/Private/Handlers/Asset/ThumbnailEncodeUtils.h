// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"

namespace PinWrightThumbnail
{
    // Image-format names as they appear in the RPC response's `format` field.
    namespace Format
    {
        inline constexpr TCHAR Png[] = TEXT("png");
        inline constexpr TCHAR Jpeg[] = TEXT("jpeg");
    }

    // Picks the encoder from the output file's extension: ".jpg" / ".jpeg" (case
    // insensitive) -> "jpeg", everything else -> "png". "Everything else" deliberately
    // includes an absent extension and an unrecognized one — PNG is the lossless default
    // a thumbnail/contact-sheet workflow wants, and defaulting is friendlier than
    // rejecting a path the caller merely forgot to suffix.
    FString FormatForOutputPath(const FString& OutputPath);

    // Encode a raw BGRA8 bitmap (FColor is laid out B,G,R,A in memory) to image bytes
    // whose format matches OutputPath's extension per FormatForOutputPath, and report
    // that format in OutFormat. This is the ONLY encode entry point asset.generate_thumbnail
    // has, which is deliberate on two counts:
    //
    //  1. Format. It never calls FImageUtils::ThumbnailCompressImageArray, which emits
    //     JPEG for any image >= 8x8 (engine USE_JPEG_FOR_THUMBNAILS) regardless of the
    //     caller's extension — that is how the verb used to write JPEG/JFIF bytes into a
    //     caller's .png path and still report success (B-thumbnail-png-writes-jpeg). PNG
    //     goes through PinWrightScreenshotUtils::EncodeBitmapToPng (the plugin's shared
    //     FColor -> PNG encoder); JPEG goes through IImageWrapper explicitly.
    //
    //  2. Alpha. Bitmap is taken by NON-CONST reference and stamped opaque in place via
    //     PinWrightScreenshotUtils::ForceOpaqueAlpha before encoding, so a production
    //     caller cannot reach the encoder without the stamp. ThumbnailTools::RenderThumbnail
    //     clears its canvas to opaque black but performs no alpha fix-up after the scene
    //     render, so whatever the renderer left in alpha survives — the exact shape of the
    //     ~99.97%-transparent-PNG defect (B-horizontal-orthographic-views-render-no-geometry).
    //     A thumbnail is an opaque frame with nothing to preserve, and the old JPEG encode
    //     masked the problem only because JPEG has no alpha channel at all.
    //
    // Returns true and fills OutBytes on success; false (OutBytes emptied, OutFormat still
    // set) on encode failure or a degenerate/mismatched bitmap (Width/Height <= 0, or fewer
    // than Width*Height pixels). Headless-callable — no GEngine, no viewport, no RHI — so
    // both contracts above are unit-testable without a live editor.
    bool EncodeByExtension(const FString& OutputPath, int32 Width, int32 Height,
        TArray<FColor>& Bitmap, TArray<uint8>& OutBytes, FString& OutFormat);
}
