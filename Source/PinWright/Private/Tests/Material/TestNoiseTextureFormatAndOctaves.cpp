// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression tests for three pixel defects in texture.create_noise_texture:
//
//   B-noise-texture-hdr-writes-bgra8 - hdr:true initialised the texture source as
//   TSF_RGBA16F (four FFloat16 channels, eight bytes per pixel) while the fill loop went on
//   writing four BGRA bytes per pixel unconditionally. Two consequences, both visible in the
//   pixels: only the first half of the buffer was ever written, and the half that was got
//   reinterpreted a byte pair at a time as half-floats, so a B/G pair became one channel and
//   the constant alpha byte became part of the next. The verb advertised an HDR texture and
//   produced neither the field it was asked for nor a fully written image.
//
//   B-noise-texture-octaves-zero-nan - octaves:0 ran the FBM accumulation zero times, so its
//   normaliser stayed at zero and the result was 0/0. The NaN did not reach the buffer as a
//   NaN: FMath::Clamp is Max(Min(X, Hi), Lo) and every comparison against a NaN is false, so
//   Clamp(NaN, 0, 1) returns 1 on every engine version in the supported range. It reached the
//   buffer as a flat white image reported as a plain success - which is the worse failure,
//   because nothing in the response distinguishes it from a legitimate texture.
//
//   B-fbm-noise-divides-by-zero-on-negative-persistence - the second route into that same 0/0,
//   left open once the octave range was closed: a negative persistence alternates the sign of
//   the amplitude terms, so with an even octave count they cancel to exactly zero and the
//   normalisation divides by it. Same laundered NaN, same flat white image reported as a
//   success.
//
// All three are pixel defects, so the tests read what the verb actually wrote out of the
// editable source mip rather than settling for "an asset was produced". The HDR test's
// counterfactual is exact: under the byte-writing fill the alpha channel was assembled out of
// unrelated bytes and could not read 1.0, and the half-float field could not agree with the
// byte field the same parameters produce. The octaves and persistence tests share their
// counterfactual - that a texture existed at all: pre-fix octaves:0, and persistence:-1 with
// an even octave count, each left behind a uniformly white asset and reported success.

#include "Misc/AutomationTest.h"
#include "Engine/Texture2D.h"
#include "Engine/TextureDefines.h"
#include "Math/Float16.h"

#include "Tests/TestUtils.h"

// Named rather than anonymous: this file is Unity-merged with its neighbours, where two
// anonymous namespaces exposing the same symbol collide. Every name here is also distinct
// from the ones in TestNoiseTextureAlgorithmAndTiling.cpp, whose file-scope
// `using namespace` is visible in this translation unit after a merge - a shared name would
// become ambiguous rather than shadowed.
namespace NoiseTextureFormatTestHelpers
{
    // A disposable package path per test asset; save:false keeps every one of these in memory
    // only, so nothing reaches disk and nothing needs deleting from it.
    static const TCHAR* const FormatTestFolder = TEXT("/Game/PinWrightTests/Texture");

    // One RGBA sample, read back as float regardless of the source format, so the byte path
    // and the half-float path can be compared to each other directly.
    struct FNoiseSample
    {
        float R = 0.0f;
        float G = 0.0f;
        float B = 0.0f;
        float A = 0.0f;
    };

    // The decoded image, in the order the verb wrote it.
    struct FNoiseSamples
    {
        int32 Width = 0;
        int32 Height = 0;
        TArray<FNoiseSample> Pixels;

        bool IsValid() const
        {
            return Width > 0 && Height > 0 && Pixels.Num() == Width * Height;
        }
    };

    // A unique asset name, so repeated runs in one editor never collide on a package.
    FString MakeFormatAssetName(const TCHAR* Prefix)
    {
        return FString::Printf(TEXT("T_%s_%s"), Prefix, *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    }

    // Builds a create_noise_texture payload. Square textures only, and small: these tests read
    // every pixel, and the field's shape does not depend on the resolution.
    TSharedPtr<FJsonObject> MakeFormatPayload(const FString& AssetName, int32 Size, int32 Octaves,
        int32 Seed, bool bHDR)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("name"), AssetName);
        Payload->SetStringField(TEXT("path"), FormatTestFolder);
        Payload->SetStringField(TEXT("noiseType"), TEXT("Perlin"));
        Payload->SetNumberField(TEXT("width"), Size);
        Payload->SetNumberField(TEXT("height"), Size);
        Payload->SetNumberField(TEXT("scale"), 4.0);
        Payload->SetNumberField(TEXT("octaves"), Octaves);
        Payload->SetNumberField(TEXT("seed"), Seed);
        Payload->SetBoolField(TEXT("seamless"), false);
        Payload->SetBoolField(TEXT("hdr"), bHDR);
        Payload->SetBoolField(TEXT("save"), false);
        return Payload;
    }

    // The same payload with an explicit persistence, which the payload above leaves at the
    // verb's 0.5 default.
    TSharedPtr<FJsonObject> MakeFormatPayloadWithPersistence(const FString& AssetName, int32 Size,
        int32 Octaves, int32 Seed, bool bHDR, double Persistence)
    {
        TSharedPtr<FJsonObject> Payload = MakeFormatPayload(AssetName, Size, Octaves, Seed, bHDR);
        Payload->SetNumberField(TEXT("persistence"), Persistence);
        return Payload;
    }

    // The in-memory texture the verb just created, or null.
    UTexture2D* FindFormatAsset(const FString& AssetName)
    {
        const FString PackagePath = FString::Printf(TEXT("%s/%s"), FormatTestFolder, *AssetName);
        return Cast<UTexture2D>(
            StaticFindObject(UTexture2D::StaticClass(), nullptr, *ToObjectPath(PackagePath)));
    }

    // Leaves the in-memory package clean behind us: save:false wrote nothing to disk, so
    // clearing the dirty flag is the whole teardown.
    void DeDirtyFormatAsset(const FString& AssetName)
    {
        if (UTexture2D* Created = FindFormatAsset(AssetName))
        {
            if (UPackage* Pkg = Created->GetOutermost())
            {
                Pkg->SetDirtyFlag(false);
            }
        }
    }

    // Reads the editable source mip - the same buffer the verb wrote, so no streaming or
    // platform-compression stage can launder a wrong pixel into a right one - decoding
    // whichever of the two formats create_noise_texture allocates. Returns false for any
    // other format rather than guessing at a stride.
    bool ReadSourceSamples(UTexture2D* Texture, FNoiseSamples& OutSamples)
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
            FNoiseSample& Sample = OutSamples.Pixels[static_cast<int32>(Index)];
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
    bool GenerateFormatSamples(FAutomationTestBase& Test, const FString& AssetName,
        const TSharedPtr<FJsonObject>& Payload, FNoiseSamples& OutSamples,
        FTestResponseCapture& OutCapture)
    {
        const bool bFound = InvokeHandlerWithCapture(
            TEXT("texture.create_noise_texture"), Payload, OutCapture);
        Test.TestTrue(TEXT("texture.create_noise_texture is registered"), bFound);
        Test.TestTrue(TEXT("texture.create_noise_texture responded"), OutCapture.bWasCalled);
        if (!Test.TestTrue(TEXT("texture.create_noise_texture succeeded"), OutCapture.bSuccess))
        {
            return false;
        }

        UTexture2D* Created = FindFormatAsset(AssetName);
        if (!Test.TestNotNull(TEXT("the generated texture is findable"), Created))
        {
            return false;
        }

        const bool bRead = ReadSourceSamples(Created, OutSamples);
        Test.TestTrue(TEXT("the generated texture's source mip is readable"), bRead);
        return bRead && OutSamples.IsValid();
    }

    // Reads the boolean the response echoed back, or leaves the default when absent.
    bool EchoedBool(const FTestResponseCapture& Capture, const TCHAR* Field, bool bDefault)
    {
        bool bValue = bDefault;
        if (Capture.Result.IsValid())
        {
            Capture.Result->TryGetBoolField(Field, bValue);
        }
        return bValue;
    }
}

// hdr:true must fill the RGBA16F source it allocates with half-floats, and the field it draws
// must be the same field the byte path draws from the same parameters.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNoiseTextureHdrWritesHalfFloatPixelsTest,
    "PinWright.texture.create_noise_texture.HdrWritesHalfFloatPixels",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNoiseTextureHdrWritesHalfFloatPixelsTest::RunTest(const FString& Parameters)
{
    using namespace NoiseTextureFormatTestHelpers;

    const int32 Size = 32;
    const int32 Octaves = 3;
    const int32 Seed = 4242;

    const FString HdrName = MakeFormatAssetName(TEXT("NoiseHdr"));
    const FString LdrName = MakeFormatAssetName(TEXT("NoiseLdr"));

    FNoiseSamples HdrSamples;
    FNoiseSamples LdrSamples;
    FTestResponseCapture HdrCapture;
    FTestResponseCapture LdrCapture;

    const bool bHdrOk = GenerateFormatSamples(*this, HdrName,
        MakeFormatPayload(HdrName, Size, Octaves, Seed, /*bHDR=*/true), HdrSamples, HdrCapture);
    const bool bLdrOk = GenerateFormatSamples(*this, LdrName,
        MakeFormatPayload(LdrName, Size, Octaves, Seed, /*bHDR=*/false), LdrSamples, LdrCapture);

    UTexture2D* HdrTexture = FindFormatAsset(HdrName);
    UTexture2D* LdrTexture = FindFormatAsset(LdrName);

    // The allocation half of the contract, so a later regression that silently drops back to a
    // byte source is caught by the format assertion rather than only by the pixel ones.
    if (HdrTexture)
    {
        TestEqual(TEXT("hdr:true allocates a TSF_RGBA16F source"),
            static_cast<int32>(HdrTexture->Source.GetFormat()), static_cast<int32>(TSF_RGBA16F));
        TestEqual(TEXT("the RGBA16F source is eight bytes per pixel"),
            HdrTexture->Source.GetBytesPerPixel(), static_cast<int64>(8));
        TestFalse(TEXT("the HDR texture is linear, not sRGB"), static_cast<bool>(HdrTexture->SRGB));
    }
    if (LdrTexture)
    {
        TestEqual(TEXT("hdr:false still allocates a TSF_BGRA8 source"),
            static_cast<int32>(LdrTexture->Source.GetFormat()), static_cast<int32>(TSF_BGRA8));
    }

    // The response reports the format the pixels were written in, so a caller can tell without
    // loading the asset.
    TestTrue(TEXT("the response reports hdr:true for the HDR texture"),
        EchoedBool(HdrCapture, TEXT("hdr"), false));
    TestFalse(TEXT("the response reports hdr:false for the byte texture"),
        EchoedBool(LdrCapture, TEXT("hdr"), true));

    DeDirtyFormatAsset(HdrName);
    DeDirtyFormatAsset(LdrName);

    if (!bHdrOk || !bLdrOk)
    {
        return false;
    }
    if (!TestEqual(TEXT("both textures are the same size"), HdrSamples.Pixels.Num(), LdrSamples.Pixels.Num()))
    {
        return false;
    }

    // 1. Every pixel of the half-float buffer is a written half-float. Under the byte-writing
    //    fill this failed on the whole image at once: alpha was assembled out of a neighbouring
    //    pixel's colour bytes and could not read 1.0, the colour channels were byte pairs
    //    reinterpreted as halves, and the second half of the buffer was never written at all.
    int32 NonFinite = 0;
    int32 OutOfRange = 0;
    int32 NotGrayscale = 0;
    int32 AlphaNotOpaque = 0;
    float MinValue = TNumericLimits<float>::Max();
    float MaxValue = -TNumericLimits<float>::Max();
    for (const FNoiseSample& Sample : HdrSamples.Pixels)
    {
        if (!FMath::IsFinite(Sample.R) || !FMath::IsFinite(Sample.G) ||
            !FMath::IsFinite(Sample.B) || !FMath::IsFinite(Sample.A))
        {
            ++NonFinite;
            continue;
        }
        if (Sample.R < 0.0f || Sample.R > 1.0f)
        {
            ++OutOfRange;
        }
        if (Sample.R != Sample.G || Sample.G != Sample.B)
        {
            ++NotGrayscale;
        }
        if (!FMath::IsNearlyEqual(Sample.A, 1.0f, 1.0e-3f))
        {
            ++AlphaNotOpaque;
        }
        MinValue = FMath::Min(MinValue, Sample.R);
        MaxValue = FMath::Max(MaxValue, Sample.R);
    }
    TestEqual(TEXT("no HDR pixel is NaN or infinite"), NonFinite, 0);
    TestEqual(TEXT("every HDR pixel stays inside the normalised 0-1 range"), OutOfRange, 0);
    TestEqual(TEXT("every HDR pixel is grayscale, as the byte path is"), NotGrayscale, 0);
    TestEqual(TEXT("every HDR pixel is written opaque"), AlphaNotOpaque, 0);

    // 2. It is a field, not a constant - the whole buffer was written, not just the part a
    //    four-byte stride reaches.
    TestTrue(*FString::Printf(TEXT("the HDR field varies across the image (range %.4f to %.4f)"),
        MinValue, MaxValue), (MaxValue - MinValue) > 0.05f);

    // 3. It is the SAME field the byte path draws from the same parameters, to within the
    //    quantisation the byte path applies. This is what separates "wrote half-floats" from
    //    "wrote half-floats of something else": the pre-fix buffer's values came from
    //    reinterpreting BGRA byte pairs, which bears no relation to the noise value.
    int32 Disagreeing = 0;
    for (int32 Index = 0; Index < HdrSamples.Pixels.Num(); ++Index)
    {
        const float HdrByte = static_cast<float>(static_cast<uint8>(
            FMath::Clamp(HdrSamples.Pixels[Index].R, 0.0f, 1.0f) * 255.0f));
        if (FMath::Abs(HdrByte - LdrSamples.Pixels[Index].R) > 1.0f)
        {
            ++Disagreeing;
        }
    }
    TestEqual(TEXT("the HDR field matches the byte field within one quantisation step"),
        Disagreeing, 0);

    return true;
}

// octaves outside the range the FBM is defined for must be refused - naming the range - and
// must not leave a texture behind; the smallest legal count must still produce finite pixels.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNoiseTextureOctavesRangeIsEnforcedTest,
    "PinWright.texture.create_noise_texture.OctavesRangeIsEnforced",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNoiseTextureOctavesRangeIsEnforcedTest::RunTest(const FString& Parameters)
{
    using namespace NoiseTextureFormatTestHelpers;

    // The defect: octaves:0 ran the accumulation zero times, normalised by the zero that left
    // behind, and shipped the clamped NaN as a uniformly white texture reported as a success.
    // Rejecting is what keeps a caller who meant "flat" from being handed one silently.
    const int32 RejectedCounts[] = { 0, -3, 17 };
    for (const int32 Rejected : RejectedCounts)
    {
        const FString AssetName = MakeFormatAssetName(TEXT("NoiseOctaves"));

        FTestResponseCapture Capture;
        const bool bFound = InvokeHandlerWithCapture(TEXT("texture.create_noise_texture"),
            MakeFormatPayload(AssetName, 32, Rejected, 11, /*bHDR=*/false), Capture);
        TestTrue(TEXT("texture.create_noise_texture is registered"), bFound);
        TestTrue(TEXT("texture.create_noise_texture responded"), Capture.bWasCalled);

        TestFalse(*FString::Printf(TEXT("octaves:%d is refused rather than silently substituted"),
            Rejected), Capture.bSuccess);
        TestTrue(*FString::Printf(TEXT("the refusal for octaves:%d names the parameter (message: %s)"),
            Rejected, *Capture.Message), Capture.Message.Contains(TEXT("octaves")));
        TestTrue(*FString::Printf(TEXT("the refusal for octaves:%d names the valid range (message: %s)"),
            Rejected, *Capture.Message), Capture.Message.Contains(TEXT("between 1 and 16")));
        TestTrue(*FString::Printf(TEXT("the refusal for octaves:%d reports what was passed (message: %s)"),
            Rejected, *Capture.Message),
            Capture.Message.Contains(FString::Printf(TEXT("got %d"), Rejected)));

        // The rejection happens before anything is created, so there is no buffer of laundered
        // NaN left over for a caller to pick up and no half-made asset to clean away.
        TestNull(*FString::Printf(TEXT("no texture is created for octaves:%d"), Rejected),
            FindFormatAsset(AssetName));
    }

    // The boundary the old code turned into 0/0 must work, and must produce a buffer with no
    // NaN in it. Read as half-floats, where a NaN would survive as a NaN rather than being
    // rounded into a byte.
    const FString BoundaryName = MakeFormatAssetName(TEXT("NoiseOneOctave"));
    FNoiseSamples BoundarySamples;
    FTestResponseCapture BoundaryCapture;
    const bool bBoundaryOk = GenerateFormatSamples(*this, BoundaryName,
        MakeFormatPayload(BoundaryName, 32, /*Octaves=*/1, 11, /*bHDR=*/true),
        BoundarySamples, BoundaryCapture);

    DeDirtyFormatAsset(BoundaryName);

    if (bBoundaryOk)
    {
        int32 NonFinite = 0;
        int32 Uniform = 0;
        const float FirstValue = BoundarySamples.Pixels[0].R;
        for (const FNoiseSample& Sample : BoundarySamples.Pixels)
        {
            if (!FMath::IsFinite(Sample.R) || !FMath::IsFinite(Sample.A))
            {
                ++NonFinite;
            }
            else if (Sample.R == FirstValue)
            {
                ++Uniform;
            }
        }
        TestEqual(TEXT("one octave produces no NaN or infinite pixel"), NonFinite, 0);
        // The pre-fix octaves:0 output was every pixel identical (the clamped NaN). One octave
        // must not look like that, or the assertion above would pass on the broken output too.
        TestTrue(*FString::Printf(
            TEXT("one octave produces a varying field, not the flat image octaves:0 used to yield ")
            TEXT("(%d of %d pixels share the first value)"), Uniform, BoundarySamples.Pixels.Num()),
            Uniform < BoundarySamples.Pixels.Num());
    }

    // The parameter description is what the generated method page shows a caller for this
    // parameter, so it must advertise the range that is actually enforced.
    if (const FParamSpec* Spec = GetRegisteredParamSpec(TEXT("texture.create_noise_texture"), TEXT("octaves")))
    {
        TestTrue(*FString::Printf(TEXT("the octaves description documents the range (description: %s)"),
            *Spec->Description),
            Spec->Description.Contains(TEXT("1")) && Spec->Description.Contains(TEXT("16")));
    }
    else
    {
        AddError(TEXT("texture.create_noise_texture declares no octaves parameter"));
    }

    return true;
}

// persistence outside the range the FBM is defined for must be refused - naming the range - and
// must not leave a texture behind; both ends of the legal range must still draw a real field.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNoiseTexturePersistenceRangeIsEnforcedTest,
    "PinWright.texture.create_noise_texture.PersistenceRangeIsEnforced",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNoiseTexturePersistenceRangeIsEnforcedTest::RunTest(const FString& Parameters)
{
    using namespace NoiseTextureFormatTestHelpers;

    // The defect: persistence:-1 alternates the sign of the amplitude terms, so on an EVEN
    // octave count they cancel to exactly zero and the normalisation divides by that zero -
    // the second route into the 0/0 the octave range closed. The clamped NaN shipped as a
    // uniformly white texture reported as a success, so every octave count below is even and
    // pre-fix every one of these calls succeeded and left an asset behind.
    const double RejectedPersistences[] = { -1.0, -0.25, 1.5 };
    for (const double Rejected : RejectedPersistences)
    {
        const FString AssetName = MakeFormatAssetName(TEXT("NoisePersistence"));

        FTestResponseCapture Capture;
        const bool bFound = InvokeHandlerWithCapture(TEXT("texture.create_noise_texture"),
            MakeFormatPayloadWithPersistence(AssetName, 32, /*Octaves=*/4, 11, /*bHDR=*/false,
                Rejected), Capture);
        TestTrue(TEXT("texture.create_noise_texture is registered"), bFound);
        TestTrue(TEXT("texture.create_noise_texture responded"), Capture.bWasCalled);

        TestFalse(*FString::Printf(TEXT("persistence:%g is refused rather than silently accepted"),
            Rejected), Capture.bSuccess);
        TestTrue(*FString::Printf(TEXT("the refusal for persistence:%g names the parameter (message: %s)"),
            Rejected, *Capture.Message), Capture.Message.Contains(TEXT("persistence")));
        TestTrue(*FString::Printf(TEXT("the refusal for persistence:%g names the valid range (message: %s)"),
            Rejected, *Capture.Message), Capture.Message.Contains(TEXT("between 0 and 1")));
        TestTrue(*FString::Printf(TEXT("the refusal for persistence:%g reports what was passed (message: %s)"),
            Rejected, *Capture.Message),
            Capture.Message.Contains(FString::Printf(TEXT("got %g"), Rejected)));

        // Refused before anything is created, so there is no white buffer for a caller to pick
        // up and no half-made asset to clean away.
        TestNull(*FString::Printf(TEXT("no texture is created for persistence:%g"), Rejected),
            FindFormatAsset(AssetName));
    }

    // Both ends of the legal range must still work and must still draw a field: persistence:0
    // leaves only the base octave with any amplitude, persistence:1 gives every octave the same
    // one. Read as half-floats, where a NaN would survive as a NaN rather than being rounded
    // into a byte.
    const double AcceptedPersistences[] = { 0.0, 1.0 };
    for (const double Accepted : AcceptedPersistences)
    {
        const FString AssetName = MakeFormatAssetName(TEXT("NoisePersistenceOk"));

        FNoiseSamples Samples;
        FTestResponseCapture Capture;
        const bool bOk = GenerateFormatSamples(*this, AssetName,
            MakeFormatPayloadWithPersistence(AssetName, 32, /*Octaves=*/4, 11, /*bHDR=*/true,
                Accepted), Samples, Capture);

        DeDirtyFormatAsset(AssetName);

        if (!bOk)
        {
            continue;
        }

        int32 NonFinite = 0;
        int32 Uniform = 0;
        const float FirstValue = Samples.Pixels[0].R;
        for (const FNoiseSample& Sample : Samples.Pixels)
        {
            if (!FMath::IsFinite(Sample.R) || !FMath::IsFinite(Sample.A))
            {
                ++NonFinite;
            }
            else if (Sample.R == FirstValue)
            {
                ++Uniform;
            }
        }
        TestEqual(*FString::Printf(TEXT("persistence:%g produces no NaN or infinite pixel"), Accepted),
            NonFinite, 0);
        // The zero-sum output was every pixel identical, so a legal persistence must not look
        // like that or the assertion above would pass on the broken output too.
        TestTrue(*FString::Printf(
            TEXT("persistence:%g produces a varying field, not the flat image the cancelled sum ")
            TEXT("yields (%d of %d pixels share the first value)"),
            Accepted, Uniform, Samples.Pixels.Num()), Uniform < Samples.Pixels.Num());
    }

    // The parameter description is what the generated method page shows a caller, so it must
    // advertise the range that is actually enforced.
    if (const FParamSpec* Spec = GetRegisteredParamSpec(TEXT("texture.create_noise_texture"), TEXT("persistence")))
    {
        TestTrue(*FString::Printf(TEXT("the persistence description documents the range (description: %s)"),
            *Spec->Description),
            Spec->Description.Contains(TEXT("between 0 and 1")));
    }
    else
    {
        AddError(TEXT("texture.create_noise_texture declares no persistence parameter"));
    }

    return true;
}
