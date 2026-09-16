// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/Asset/ThumbnailEncodeUtils.h"

#include "Utils/ScreenshotUtils.h"

#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"

namespace PinWrightThumbnail
{
    FString FormatForOutputPath(const FString& OutputPath)
    {
        const FString Extension = FPaths::GetExtension(OutputPath);
        if (Extension.Equals(TEXT("jpg"), ESearchCase::IgnoreCase) ||
            Extension.Equals(TEXT("jpeg"), ESearchCase::IgnoreCase))
        {
            return Format::Jpeg;
        }
        return Format::Png;
    }

    bool EncodeByExtension(const FString& OutputPath, int32 Width, int32 Height,
        TArray<FColor>& Bitmap, TArray<uint8>& OutBytes, FString& OutFormat)
    {
        OutBytes.Reset();
        OutFormat = FormatForOutputPath(OutputPath);

        // Reject a degenerate/mismatched bitmap before either encoder reads Width*Height
        // pixels out of it (same guard, same reason, as EncodeBitmapToPng).
        if (Width <= 0 || Height <= 0 || Bitmap.Num() < Width * Height)
        {
            return false;
        }

        // Unconditional, and inside the encoder rather than at the callsite, so no future
        // caller can produce a near-fully-transparent image by forgetting it. Contract in
        // ThumbnailEncodeUtils.h and Utils/ScreenshotUtils.h.
        PinWrightScreenshotUtils::ForceOpaqueAlpha(Bitmap);

        if (OutFormat == Format::Png)
        {
            return PinWrightScreenshotUtils::EncodeBitmapToPng(Width, Height, Bitmap, OutBytes);
        }

        IImageWrapperModule& ImageWrapperModule =
            FModuleManager::LoadModuleChecked<IImageWrapperModule>(FName("ImageWrapper"));
        TSharedPtr<IImageWrapper> JpegWrapper =
            ImageWrapperModule.CreateImageWrapper(EImageFormat::JPEG);
        if (!JpegWrapper.IsValid() ||
            !JpegWrapper->SetRaw(Bitmap.GetData(), Bitmap.Num() * sizeof(FColor),
                Width, Height, ERGBFormat::BGRA, 8))
        {
            return false;
        }

        // 90 keeps the file small without visible ringing on the flat-shaded backgrounds
        // thumbnails are mostly made of; JPEG is only ever chosen because the caller asked
        // for it by extension, so lossiness is their explicit choice.
        OutBytes = JpegWrapper->GetCompressed(90);
        return OutBytes.Num() > 0;
    }
}
