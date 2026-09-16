// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for B-texture-create-gradient-hdr-writes-bgra8 - the sibling of the defect
// TestNoiseTextureFormatAndOctaves.cpp covers, in a different verb.
//
// texture.create_gradient_texture asks CreateEmptyTexture for an HDR source when hdr:true,
// which initialises it as TSF_RGBA16F - four FFloat16 channels, eight bytes per pixel - and
// then filled it with a loop that indexed at *4 and wrote four BGRA bytes unconditionally.
// Two consequences, both in the pixels: only the first half of the buffer was ever touched,
// and the half that was got reinterpreted a byte pair at a time as half-floats, so one
// pixel's B/G bytes became a colour channel and its R/A bytes became the next. The verb
// reported success and produced neither the format it was asked for nor a full image.
//
// This is a pixel defect, so the test reads what the verb actually wrote out of the editable
// source mip rather than settling for "an asset was produced". The counterfactual is exact:
// under the byte-writing fill the second half of the HDR buffer stayed zero (alpha 0, not 1),
// and the channels in the half that was written were assembled out of unrelated bytes, so
// they could not match the byte path's field from the same parameters.

#include "Misc/AutomationTest.h"
#include "Engine/Texture2D.h"
#include "Engine/TextureDefines.h"
#include "Math/Float16.h"

#include "Tests/TestUtils.h"

// Named rather than anonymous: this file is Unity-merged with its neighbours, where two
// anonymous namespaces exposing the same symbol collide. Every name here is also distinct
// from the ones in TestNoiseTextureFormatAndOctaves.cpp and
// TestNoiseTextureAlgorithmAndTiling.cpp, whose file-scope `using namespace` is visible in
// this translation unit after a merge - a shared name would become ambiguous rather than
// shadowed.
namespace GradientTextureFormatTestHelpers
{
    // A disposable package path per test asset; save:false keeps every one of these in memory
    // only, so nothing reaches disk and nothing needs deleting from it.
    static const TCHAR* const GradientTestFolder = TEXT("/Game/PinWrightTests/Texture");

    // The gradient endpoints. Every channel takes a different value at each end, so a fill
    // that writes the channels in the wrong order - or reads unrelated bytes for them - cannot
    // coincide with the correct field anywhere along the ramp.
    static const FLinearColor GradientStartColor(0.0f, 0.25f, 0.75f, 1.0f);
    static const FLinearColor GradientEndColor(1.0f, 0.5f, 0.0f, 1.0f);

    // One RGBA sample, read back as float regardless of the source format, so the byte path
    // and the half-float path can be compared to each other directly. The byte path's samples
    // stay in 0-255; the half-float path's stay in 0-1.
    struct FGradientSample
    {
        float R = 0.0f;
        float G = 0.0f;
        float B = 0.0f;
        float A = 0.0f;
    };

    // The decoded image, in the order the verb wrote it.
    struct FGradientSamples
    {
        int32 Width = 0;
        int32 Height = 0;
        TArray<FGradientSample> Pixels;

        bool IsValid() const
        {
            return Width > 0 && Height > 0 && Pixels.Num() == Width * Height;
        }
    };

    // A unique asset name, so repeated runs in one editor never collide on a package.
    FString MakeGradientAssetName(const TCHAR* Prefix)
    {
        return FString::Printf(TEXT("T_%s_%s"), Prefix, *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    }

    // Builds a create_gradient_texture payload. A Linear gradient at angle 0 ramps along X, so
    // the field varies across the image without depending on centre or radius. Square and
    // small: this test reads every pixel, and the field's shape does not depend on resolution.
    TSharedPtr<FJsonObject> MakeGradientPayload(const FString& AssetName, int32 Size, bool bHDR)
    {
        TSharedPtr<FJsonObject> StartColor = MakeShared<FJsonObject>();
        StartColor->SetNumberField(TEXT("r"), GradientStartColor.R);
        StartColor->SetNumberField(TEXT("g"), GradientStartColor.G);
        StartColor->SetNumberField(TEXT("b"), GradientStartColor.B);
        StartColor->SetNumberField(TEXT("a"), GradientStartColor.A);

        TSharedPtr<FJsonObject> EndColor = MakeShared<FJsonObject>();
        EndColor->SetNumberField(TEXT("r"), GradientEndColor.R);
        EndColor->SetNumberField(TEXT("g"), GradientEndColor.G);
        EndColor->SetNumberField(TEXT("b"), GradientEndColor.B);
        EndColor->SetNumberField(TEXT("a"), GradientEndColor.A);

        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("name"), AssetName);
        Payload->SetStringField(TEXT("path"), GradientTestFolder);
        Payload->SetStringField(TEXT("gradientType"), TEXT("Linear"));
        Payload->SetNumberField(TEXT("width"), Size);
        Payload->SetNumberField(TEXT("height"), Size);
        Payload->SetNumberField(TEXT("angle"), 0.0);
        Payload->SetObjectField(TEXT("startColor"), StartColor);
        Payload->SetObjectField(TEXT("endColor"), EndColor);
        Payload->SetBoolField(TEXT("hdr"), bHDR);
        Payload->SetBoolField(TEXT("save"), false);
        return Payload;
    }

    // The in-memory texture the verb just created, or null.
    UTexture2D* FindGradientAsset(const FString& AssetName)
    {
        const FString PackagePath = FString::Printf(TEXT("%s/%s"), GradientTestFolder, *AssetName);
        return Cast<UTexture2D>(
            StaticFindObject(UTexture2D::StaticClass(), nullptr, *ToObjectPath(PackagePath)));
    }

    // Leaves the in-memory package clean behind us: save:false wrote nothing to disk, so
    // clearing the dirty flag is the whole teardown.
    void DeDirtyGradientAsset(const FString& AssetName)
    {
        if (UTexture2D* Created = FindGradientAsset(AssetName))
        {
            if (UPackage* Pkg = Created->GetOutermost())
            {
                Pkg->SetDirtyFlag(false);
            }
        }
    }

    // Reads the editable source mip - the same buffer the verb wrote, so no streaming or
    // platform-compression stage can launder a wrong pixel into a right one - decoding
    // whichever of the two formats create_gradient_texture allocates. Returns false for any
    // other format rather than guessing at a stride.
    bool ReadGradientSamples(UTexture2D* Texture, FGradientSamples& OutSamples)
    {
        FTextureSource& Source = Texture->Source;
        if (!Source.IsValid())
        {
            return false;
        }
        const ETextureSourceFormat Format = Source.GetFormat();
        if (Format != TSF_BGRA8 && Format != TSF_RGBA16F)
        {
            return false;
        }

        OutSamples.Width = static_cast<int32>(Source.GetSizeX());
        OutSamples.Height = static_cast<int32>(Source.GetSizeY());
        const int64 NumPixels = static_cast<int64>(OutSamples.Width) * static_cast<int64>(OutSamples.Height);
        if (NumPixels <= 0)
        {
            return false;
        }

        const uint8* MipData = Source.LockMipReadOnly(0);
        if (!MipData)
        {
            return false;
        }

        OutSamples.Pixels.SetNum(static_cast<int32>(NumPixels));
        for (int64 Index = 0; Index < NumPixels; ++Index)
        {
            FGradientSample& Sample = OutSamples.Pixels[static_cast<int32>(Index)];
            if (Format == TSF_RGBA16F)
            {
                const FFloat16* Pixel = reinterpret_cast<const FFloat16*>(MipData + Index * 8);
                Sample.R = Pixel[0].GetFloat();
                Sample.G = Pixel[1].GetFloat();
                Sample.B = Pixel[2].GetFloat();
                Sample.A = Pixel[3].GetFloat();
            }
            else
            {
                const uint8* Pixel = MipData + Index * 4;
                Sample.B = static_cast<float>(Pixel[0]);
                Sample.G = static_cast<float>(Pixel[1]);
                Sample.R = static_cast<float>(Pixel[2]);
                Sample.A = static_cast<float>(Pixel[3]);
            }
        }
        Source.UnlockMip(0);
        return true;
    }

    // Drives the real registered handler through the dispatcher and reads back what it drew.
    // Returns false (with the assertion already recorded) if anything on the way failed.
    bool GenerateGradientSamples(FAutomationTestBase& Test, const FString& AssetName,
        const TSharedPtr<FJsonObject>& Payload, FGradientSamples& OutSamples)
    {
        FTestResponseCapture Capture;
        const bool bFound = InvokeHandlerWithCapture(
            TEXT("texture.create_gradient_texture"), Payload, Capture);
        Test.TestTrue(TEXT("texture.create_gradient_texture is registered"), bFound);
        Test.TestTrue(TEXT("texture.create_gradient_texture responded"), Capture.bWasCalled);
        if (!Test.TestTrue(TEXT("texture.create_gradient_texture succeeded"), Capture.bSuccess))
        {
            return false;
        }

        UTexture2D* Created = FindGradientAsset(AssetName);
        if (!Test.TestNotNull(TEXT("the generated texture is findable"), Created))
        {
            return false;
        }

        const bool bRead = ReadGradientSamples(Created, OutSamples);
        Test.TestTrue(TEXT("the generated texture's source mip is readable"), bRead);
        return bRead && OutSamples.IsValid();
    }
}

// hdr:true must fill the RGBA16F source it allocates with half-floats, and the gradient it
// draws must be the same gradient the byte path draws from the same parameters.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGradientTextureHdrWritesHalfFloatPixelsTest,
    "PinWright.texture.create_gradient_texture.HdrWritesHalfFloatPixels",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGradientTextureHdrWritesHalfFloatPixelsTest::RunTest(const FString& Parameters)
{
    using namespace GradientTextureFormatTestHelpers;

    const int32 Size = 32;

    const FString HdrName = MakeGradientAssetName(TEXT("GradientHdr"));
    const FString LdrName = MakeGradientAssetName(TEXT("GradientLdr"));

    FGradientSamples HdrSamples;
    FGradientSamples LdrSamples;

    const bool bHdrOk = GenerateGradientSamples(*this, HdrName,
        MakeGradientPayload(HdrName, Size, /*bHDR=*/true), HdrSamples);
    const bool bLdrOk = GenerateGradientSamples(*this, LdrName,
        MakeGradientPayload(LdrName, Size, /*bHDR=*/false), LdrSamples);

    UTexture2D* HdrTexture = FindGradientAsset(HdrName);
    UTexture2D* LdrTexture = FindGradientAsset(LdrName);

    // The allocation half of the contract, so a later regression that silently drops back to a
    // byte source is caught by the format assertion rather than only by the pixel ones.
    if (HdrTexture)
    {
        TestEqual(TEXT("hdr:true allocates a TSF_RGBA16F source"),
            static_cast<int32>(HdrTexture->Source.GetFormat()), static_cast<int32>(TSF_RGBA16F));
        TestEqual(TEXT("the RGBA16F source is eight bytes per pixel"),
            HdrTexture->Source.GetBytesPerPixel(), static_cast<int64>(8));
    }
    if (LdrTexture)
    {
        TestEqual(TEXT("hdr:false still allocates a TSF_BGRA8 source"),
            static_cast<int32>(LdrTexture->Source.GetFormat()), static_cast<int32>(TSF_BGRA8));
    }

    DeDirtyGradientAsset(HdrName);
    DeDirtyGradientAsset(LdrName);

    if (!bHdrOk || !bLdrOk)
    {
        return false;
    }
    if (!TestEqual(TEXT("both textures are the same size"), HdrSamples.Pixels.Num(), LdrSamples.Pixels.Num()))
    {
        return false;
    }

    // 1. Every pixel of the half-float buffer is a written half-float. Under the byte-writing
    //    fill this failed on the whole image at once: the second half of the buffer was never
    //    written at all and read back as zero, and in the half that was, alpha was assembled
    //    out of a neighbouring pixel's colour bytes and could not read the opaque endpoint.
    int32 NonFinite = 0;
    int32 OutOfRange = 0;
    int32 AlphaNotOpaque = 0;
    for (const FGradientSample& Sample : HdrSamples.Pixels)
    {
        if (!FMath::IsFinite(Sample.R) || !FMath::IsFinite(Sample.G) ||
            !FMath::IsFinite(Sample.B) || !FMath::IsFinite(Sample.A))
        {
            ++NonFinite;
            continue;
        }
        if (Sample.R < 0.0f || Sample.R > 1.0f || Sample.G < 0.0f || Sample.G > 1.0f ||
            Sample.B < 0.0f || Sample.B > 1.0f)
        {
            ++OutOfRange;
        }
        if (!FMath::IsNearlyEqual(Sample.A, 1.0f, 1.0e-3f))
        {
            ++AlphaNotOpaque;
        }
    }
    TestEqual(TEXT("no HDR pixel is NaN or infinite"), NonFinite, 0);
    TestEqual(TEXT("every HDR pixel stays inside the endpoints' 0-1 range"), OutOfRange, 0);
    TestEqual(TEXT("every HDR pixel carries the endpoints' opaque alpha"), AlphaNotOpaque, 0);

    // 2. It is the SAME gradient the byte path draws from the same parameters, to within the
    //    quantisation the byte path applies - per channel, so a half-float fill that wrote the
    //    channels in BGRA order rather than RGBA fails here rather than passing on a
    //    grayscale coincidence. This is what separates "wrote half-floats" from "wrote
    //    half-floats of something else": the pre-fix buffer's values came from reinterpreting
    //    BGRA byte pairs, which bears no relation to the interpolated colour.
    int32 Disagreeing = 0;
    for (int32 Index = 0; Index < HdrSamples.Pixels.Num(); ++Index)
    {
        const FGradientSample& Hdr = HdrSamples.Pixels[Index];
        const FGradientSample& Ldr = LdrSamples.Pixels[Index];
        const float HdrR = static_cast<float>(static_cast<uint8>(FMath::Clamp(Hdr.R, 0.0f, 1.0f) * 255.0f));
        const float HdrG = static_cast<float>(static_cast<uint8>(FMath::Clamp(Hdr.G, 0.0f, 1.0f) * 255.0f));
        const float HdrB = static_cast<float>(static_cast<uint8>(FMath::Clamp(Hdr.B, 0.0f, 1.0f) * 255.0f));
        if (FMath::Abs(HdrR - Ldr.R) > 1.0f || FMath::Abs(HdrG - Ldr.G) > 1.0f ||
            FMath::Abs(HdrB - Ldr.B) > 1.0f)
        {
            ++Disagreeing;
        }
    }
    TestEqual(TEXT("the HDR gradient matches the byte gradient within one quantisation step"),
        Disagreeing, 0);

    // 3. It is a ramp, not a constant - the whole buffer was written, not just the part a
    //    four-byte stride reaches. Read on the red channel, which runs the full 0-1 span.
    float MinRed = TNumericLimits<float>::Max();
    float MaxRed = -TNumericLimits<float>::Max();
    for (const FGradientSample& Sample : HdrSamples.Pixels)
    {
        if (FMath::IsFinite(Sample.R))
        {
            MinRed = FMath::Min(MinRed, Sample.R);
            MaxRed = FMath::Max(MaxRed, Sample.R);
        }
    }
    TestTrue(*FString::Printf(TEXT("the HDR gradient ramps across the image (red %.4f to %.4f)"),
        MinRed, MaxRed), (MaxRed - MinRed) > 0.5f);

    return true;
}
