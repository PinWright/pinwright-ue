// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression tests for the two pixel defects in texture.create_noise_texture:
//
//   B-noise-texture-noisetype-ignored - `noiseType` was parsed into a local nothing read.
//   Every value produced byte-identical Perlin FBM and a name no algorithm answers to was
//   accepted with success, so a caller asking for cellular noise silently got value noise.
//
//   B-noise-texture-seamless-lattice - the `seamless:true` branch built the four coordinates
//   of a 4D torus and then collapsed them into two by addition before feeding a 2D noise
//   function. cos(a)+cos(b) and sin(a)+sin(b) are symmetric in their arguments and
//   non-injective over the unit square, so the only tiling-safe path emitted a periodic
//   diagonal lattice - one motif repeated on a grid - instead of noise.
//
// Both are pixel defects, so these tests read the pixels the verb actually wrote out of the
// editable source mip rather than settling for "an asset was produced". The lattice
// assertions are exact counterfactuals: under the old coordinate collapse the image was
// perfectly symmetric about its main diagonal (p(x,y) == p(y,x) for every pixel of a square
// texture, because the two summed coordinates are symmetric under swapping NX and NY) and
// perfectly periodic at width/scale pixels (the torus angles advance by exactly 2*pi over
// that span). Both hold for 100% of pixels on the pre-fix generator and for almost none on a
// genuine noise field, which is what separates the fix from the bug.

#include "Misc/AutomationTest.h"
#include "Engine/Texture2D.h"
#include "Engine/TextureDefines.h"

#include "Tests/TestUtils.h"

// Named rather than anonymous: this file is Unity-merged with its neighbours, where two
// anonymous namespaces exposing the same symbol collide.
namespace NoiseTextureTilingTestHelpers
{
    // A disposable package path per test asset; save:false keeps every one of these in memory
    // only, so nothing reaches disk and nothing needs deleting from it.
    static const TCHAR* const NoiseTestFolder = TEXT("/Game/PinWrightTests/Texture");

    // The single-channel view of a generated texture. create_noise_texture writes the same
    // byte into B, G and R, so one channel carries the whole field.
    struct FNoiseImage
    {
        int32 Width = 0;
        int32 Height = 0;
        TArray<uint8> Gray;

        bool IsValid() const
        {
            return Width > 0 && Height > 0 && Gray.Num() == Width * Height;
        }

        uint8 At(int32 X, int32 Y) const
        {
            return Gray[Y * Width + X];
        }
    };

    // Builds a create_noise_texture payload. Square textures only: the diagonal-symmetry
    // assertion below needs Width == Height to be meaningful.
    TSharedPtr<FJsonObject> MakeNoisePayload(const FString& AssetName, const FString& NoiseType,
        int32 Size, double Scale, int32 Octaves, int32 Seed, bool bSeamless)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("name"), AssetName);
        Payload->SetStringField(TEXT("path"), NoiseTestFolder);
        Payload->SetStringField(TEXT("noiseType"), NoiseType);
        Payload->SetNumberField(TEXT("width"), Size);
        Payload->SetNumberField(TEXT("height"), Size);
        Payload->SetNumberField(TEXT("scale"), Scale);
        Payload->SetNumberField(TEXT("octaves"), Octaves);
        Payload->SetNumberField(TEXT("seed"), Seed);
        Payload->SetBoolField(TEXT("seamless"), bSeamless);
        Payload->SetBoolField(TEXT("save"), false);
        return Payload;
    }

    // A unique asset name, so repeated runs in one editor never collide on a package.
    FString MakeNoiseAssetName(const TCHAR* Prefix)
    {
        return FString::Printf(TEXT("T_%s_%s"), Prefix, *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    }

    // Leaves the in-memory package clean behind us: save:false wrote nothing to disk, so
    // clearing the dirty flag is the whole teardown (the same thing the shared
    // TestCreateHandlerSaveWritesToDisk runner does on its save:false path).
    void DeDirtyNoiseAsset(const FString& AssetName)
    {
        const FString PackagePath = FString::Printf(TEXT("%s/%s"), NoiseTestFolder, *AssetName);
        if (UObject* Created = StaticFindObject(UTexture2D::StaticClass(), nullptr, *ToObjectPath(PackagePath)))
        {
            if (UPackage* Pkg = Created->GetOutermost())
            {
                Pkg->SetDirtyFlag(false);
            }
        }
    }

    // Reads the R channel of the editable source mip - the same buffer the verb wrote, so no
    // streaming or platform-compression stage can launder a wrong pixel into a right one.
    bool ReadSourceGray(UTexture2D* Texture, FNoiseImage& OutImage)
    {
        FTextureSource& Source = Texture->Source;
        if (!Source.IsValid() || Source.GetFormat() != TSF_BGRA8)
        {
            return false;
        }
        OutImage.Width = static_cast<int32>(Source.GetSizeX());
        OutImage.Height = static_cast<int32>(Source.GetSizeY());
        const int64 NumPixels = static_cast<int64>(OutImage.Width) * static_cast<int64>(OutImage.Height);
        if (NumPixels <= 0)
        {
            return false;
        }

        const uint8* MipData = Source.LockMipReadOnly(0);
        if (!MipData)
        {
            return false;
        }
        OutImage.Gray.SetNumUninitialized(static_cast<int32>(NumPixels));
        for (int64 Index = 0; Index < NumPixels; ++Index)
        {
            OutImage.Gray[static_cast<int32>(Index)] = MipData[Index * 4 + 2];
        }
        Source.UnlockMip(0);
        return true;
    }

    // Drives the real registered handler through the dispatcher and reads back what it drew.
    // Returns false (with the assertion already recorded) if anything on the way failed.
    bool GenerateNoiseImage(FAutomationTestBase& Test, const FString& AssetName,
        const TSharedPtr<FJsonObject>& Payload, FNoiseImage& OutImage,
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

        const FString PackagePath = FString::Printf(TEXT("%s/%s"), NoiseTestFolder, *AssetName);
        UTexture2D* Created = Cast<UTexture2D>(
            StaticFindObject(UTexture2D::StaticClass(), nullptr, *ToObjectPath(PackagePath)));
        if (!Test.TestNotNull(TEXT("the generated texture is findable"), Created))
        {
            return false;
        }

        const bool bRead = ReadSourceGray(Created, OutImage);
        Test.TestTrue(TEXT("the generated texture's source mip is readable"), bRead);
        return bRead && OutImage.IsValid();
    }

    // Fraction of pixels where the two images differ, over their overlapping extent.
    double DifferingFraction(const FNoiseImage& A, const FNoiseImage& B)
    {
        const int32 W = FMath::Min(A.Width, B.Width);
        const int32 H = FMath::Min(A.Height, B.Height);
        int64 Differing = 0;
        for (int32 Y = 0; Y < H; ++Y)
        {
            for (int32 X = 0; X < W; ++X)
            {
                Differing += (A.At(X, Y) != B.At(X, Y)) ? 1 : 0;
            }
        }
        return static_cast<double>(Differing) / static_cast<double>(FMath::Max(1, W * H));
    }

    // Mean |p(x,y) - p(x+1,y)| across the wrap, i.e. treating column W-1 as adjacent to
    // column 0. On a tiling image this is one ordinary step of the field; on a seamed one it
    // is a jump between unrelated values.
    double MeanWrapStepX(const FNoiseImage& Image)
    {
        double Sum = 0.0;
        for (int32 Y = 0; Y < Image.Height; ++Y)
        {
            Sum += FMath::Abs(static_cast<double>(Image.At(Image.Width - 1, Y)) -
                static_cast<double>(Image.At(0, Y)));
        }
        return Sum / static_cast<double>(FMath::Max(1, Image.Height));
    }

    // Same across the top/bottom wrap.
    double MeanWrapStepY(const FNoiseImage& Image)
    {
        double Sum = 0.0;
        for (int32 X = 0; X < Image.Width; ++X)
        {
            Sum += FMath::Abs(static_cast<double>(Image.At(X, Image.Height - 1)) -
                static_cast<double>(Image.At(X, 0)));
        }
        return Sum / static_cast<double>(FMath::Max(1, Image.Width));
    }

    // The largest mean step over any interior column boundary. Used as the yardstick the wrap
    // step must not exceed: on an exactly tiling field the wrap boundary is drawn from this
    // same population, so comparing against the maximum (not the mean) leaves generous room
    // for the field being locally steeper at the edge than on average.
    double MaxInteriorColumnStep(const FNoiseImage& Image)
    {
        double Worst = 0.0;
        for (int32 X = 0; X + 1 < Image.Width; ++X)
        {
            double Sum = 0.0;
            for (int32 Y = 0; Y < Image.Height; ++Y)
            {
                Sum += FMath::Abs(static_cast<double>(Image.At(X, Y)) -
                    static_cast<double>(Image.At(X + 1, Y)));
            }
            Worst = FMath::Max(Worst, Sum / static_cast<double>(FMath::Max(1, Image.Height)));
        }
        return Worst;
    }

    // The largest mean step over any interior row boundary.
    double MaxInteriorRowStep(const FNoiseImage& Image)
    {
        double Worst = 0.0;
        for (int32 Y = 0; Y + 1 < Image.Height; ++Y)
        {
            double Sum = 0.0;
            for (int32 X = 0; X < Image.Width; ++X)
            {
                Sum += FMath::Abs(static_cast<double>(Image.At(X, Y)) -
                    static_cast<double>(Image.At(X, Y + 1)));
            }
            Worst = FMath::Max(Worst, Sum / static_cast<double>(FMath::Max(1, Image.Width)));
        }
        return Worst;
    }

    // Fraction of pixels that differ from their mirror across the main diagonal. The pre-fix
    // seamless generator summed its torus coordinate pairs, which are symmetric under
    // swapping NX and NY, so this was exactly 0 for every square texture it produced.
    double DiagonallyAsymmetricFraction(const FNoiseImage& Image)
    {
        const int32 Size = FMath::Min(Image.Width, Image.Height);
        int64 Asymmetric = 0;
        int64 Total = 0;
        for (int32 Y = 0; Y < Size; ++Y)
        {
            for (int32 X = 0; X < Size; ++X)
            {
                if (X == Y)
                {
                    continue;
                }
                Asymmetric += (Image.At(X, Y) != Image.At(Y, X)) ? 1 : 0;
                ++Total;
            }
        }
        return static_cast<double>(Asymmetric) / static_cast<double>(FMath::Max<int64>(1, Total));
    }

    // Fraction of pixels that differ from the pixel Period columns to their right. The pre-fix
    // seamless generator repeated its motif exactly every width/scale pixels, so this was
    // exactly 0; a genuine noise field has independent lattice cells and differs almost
    // everywhere.
    double SubTilePeriodBreakFraction(const FNoiseImage& Image, int32 Period)
    {
        int64 Differing = 0;
        int64 Total = 0;
        for (int32 Y = 0; Y < Image.Height; ++Y)
        {
            for (int32 X = 0; X + Period < Image.Width; ++X)
            {
                Differing += (Image.At(X, Y) != Image.At(X + Period, Y)) ? 1 : 0;
                ++Total;
            }
        }
        return static_cast<double>(Differing) / static_cast<double>(FMath::Max<int64>(1, Total));
    }
}

using namespace NoiseTextureTilingTestHelpers;

// noiseType must select the algorithm: two supported values must draw different fields, and
// the documented alias must resolve to the field it names.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNoiseTextureNoiseTypeSelectsAlgorithmTest,
    "PinWright.texture.create_noise_texture.NoiseTypeSelectsAlgorithm",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNoiseTextureNoiseTypeSelectsAlgorithmTest::RunTest(const FString& Parameters)
{
    const int32 Size = 64;
    const double Scale = 4.0;
    const int32 Octaves = 3;
    const int32 Seed = 7;

    const FString PerlinName = MakeNoiseAssetName(TEXT("NoisePerlin"));
    const FString WorleyName = MakeNoiseAssetName(TEXT("NoiseWorley"));
    const FString VoronoiName = MakeNoiseAssetName(TEXT("NoiseVoronoi"));

    FNoiseImage PerlinImage;
    FNoiseImage WorleyImage;
    FNoiseImage VoronoiImage;
    FTestResponseCapture PerlinCapture;
    FTestResponseCapture WorleyCapture;
    FTestResponseCapture VoronoiCapture;

    const bool bPerlin = GenerateNoiseImage(*this, PerlinName,
        MakeNoisePayload(PerlinName, TEXT("Perlin"), Size, Scale, Octaves, Seed, /*bSeamless=*/false),
        PerlinImage, PerlinCapture);
    const bool bWorley = GenerateNoiseImage(*this, WorleyName,
        MakeNoisePayload(WorleyName, TEXT("Worley"), Size, Scale, Octaves, Seed, /*bSeamless=*/false),
        WorleyImage, WorleyCapture);
    const bool bVoronoi = GenerateNoiseImage(*this, VoronoiName,
        MakeNoisePayload(VoronoiName, TEXT("Voronoi"), Size, Scale, Octaves, Seed, /*bSeamless=*/false),
        VoronoiImage, VoronoiCapture);

    DeDirtyNoiseAsset(PerlinName);
    DeDirtyNoiseAsset(WorleyName);
    DeDirtyNoiseAsset(VoronoiName);

    if (!bPerlin || !bWorley || !bVoronoi)
    {
        return false;
    }

    // The defect: every noiseType produced byte-identical Perlin FBM, so this fraction was 0.
    const double Differing = DifferingFraction(PerlinImage, WorleyImage);
    TestTrue(*FString::Printf(
        TEXT("Perlin and Worley draw different fields from identical parameters (differing fraction %.4f)"),
        Differing), Differing > 0.5);

    // The alias is the same field, not a third one - and the response says which field it is,
    // so a caller who spelled it "Voronoi" can see what was generated.
    TestEqual(TEXT("Voronoi is the same field as Worley"),
        DifferingFraction(VoronoiImage, WorleyImage), 0.0);

    FString EchoedPerlin;
    FString EchoedVoronoi;
    if (PerlinCapture.Result.IsValid())
    {
        PerlinCapture.Result->TryGetStringField(TEXT("noiseType"), EchoedPerlin);
    }
    if (VoronoiCapture.Result.IsValid())
    {
        VoronoiCapture.Result->TryGetStringField(TEXT("noiseType"), EchoedVoronoi);
    }
    TestEqual(TEXT("the response echoes the resolved algorithm for Perlin"), EchoedPerlin, TEXT("Perlin"));
    TestEqual(TEXT("the response resolves the Voronoi alias to Worley"), EchoedVoronoi, TEXT("Worley"));

    return true;
}

// An unrecognised noiseType must be refused - naming the set that would work - and must not
// leave an asset behind.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNoiseTextureUnknownNoiseTypeIsRefusedTest,
    "PinWright.texture.create_noise_texture.UnknownNoiseTypeIsRefused",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNoiseTextureUnknownNoiseTypeIsRefusedTest::RunTest(const FString& Parameters)
{
    const FString AssetName = MakeNoiseAssetName(TEXT("NoiseBogus"));

    FTestResponseCapture Capture;
    const bool bFound = InvokeHandlerWithCapture(TEXT("texture.create_noise_texture"),
        MakeNoisePayload(AssetName, TEXT("ZZZNotARealNoise"), 32, 4.0, 2, 7, /*bSeamless=*/false),
        Capture);
    TestTrue(TEXT("texture.create_noise_texture is registered"), bFound);
    TestTrue(TEXT("texture.create_noise_texture responded"), Capture.bWasCalled);

    // The defect: a name no algorithm answers to was accepted, an asset was created, and the
    // caller's typo came back as a success.
    TestFalse(TEXT("an unknown noiseType is refused rather than silently substituted"), Capture.bSuccess);
    TestTrue(*FString::Printf(TEXT("the refusal names Perlin (message: %s)"), *Capture.Message),
        Capture.Message.Contains(TEXT("Perlin")));
    TestTrue(*FString::Printf(TEXT("the refusal names Worley (message: %s)"), *Capture.Message),
        Capture.Message.Contains(TEXT("Worley")));
    TestTrue(*FString::Printf(TEXT("the refusal names Voronoi (message: %s)"), *Capture.Message),
        Capture.Message.Contains(TEXT("Voronoi")));

    // The rejection happens before anything is created, so no half-made asset is left over.
    const FString PackagePath = FString::Printf(TEXT("%s/%s"), NoiseTestFolder, *AssetName);
    TestNull(TEXT("no texture is created for a refused noiseType"),
        StaticFindObject(UTexture2D::StaticClass(), nullptr, *ToObjectPath(PackagePath)));

    // The parameter description is the wiki prose for this verb (docs/wiki-src/texture.md
    // carries no per-method section), so it must advertise exactly the set that is accepted.
    if (const FParamSpec* Spec = GetRegisteredParamSpec(TEXT("texture.create_noise_texture"), TEXT("noiseType")))
    {
        TestTrue(TEXT("the noiseType description documents Perlin"), Spec->Description.Contains(TEXT("Perlin")));
        TestTrue(TEXT("the noiseType description documents Worley"), Spec->Description.Contains(TEXT("Worley")));
        TestTrue(TEXT("the noiseType description documents Voronoi"), Spec->Description.Contains(TEXT("Voronoi")));
    }
    else
    {
        AddError(TEXT("texture.create_noise_texture declares no noiseType parameter"));
    }

    return true;
}

// seamless:true must tile AND still be noise. Continuity alone does not distinguish the fix
// from the bug - the collapsed-coordinate version tiled too; it just was not noise.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNoiseTextureSeamlessTilesWithoutLatticeTest,
    "PinWright.texture.create_noise_texture.SeamlessTilesWithoutLattice",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNoiseTextureSeamlessTilesWithoutLatticeTest::RunTest(const FString& Parameters)
{
    const int32 Size = 64;
    const double Scale = 4.0;
    const int32 Octaves = 3;
    const int32 Seed = 909;

    const FString SeamlessName = MakeNoiseAssetName(TEXT("NoiseSeamless"));
    const FString SeamedName = MakeNoiseAssetName(TEXT("NoiseSeamed"));

    FNoiseImage SeamlessImage;
    FNoiseImage SeamedImage;
    FTestResponseCapture SeamlessCapture;
    FTestResponseCapture SeamedCapture;

    const bool bSeamless = GenerateNoiseImage(*this, SeamlessName,
        MakeNoisePayload(SeamlessName, TEXT("Perlin"), Size, Scale, Octaves, Seed, /*bSeamless=*/true),
        SeamlessImage, SeamlessCapture);
    const bool bSeamed = GenerateNoiseImage(*this, SeamedName,
        MakeNoisePayload(SeamedName, TEXT("Perlin"), Size, Scale, Octaves, Seed, /*bSeamless=*/false),
        SeamedImage, SeamedCapture);

    DeDirtyNoiseAsset(SeamlessName);
    DeDirtyNoiseAsset(SeamedName);

    if (!bSeamless || !bSeamed)
    {
        return false;
    }

    // 1. It tiles: the wrap boundary must be an ordinary step of the field, not a jump. The
    //    yardstick is the worst interior boundary of the same image, so a locally steep edge
    //    cannot fail it.
    const double WrapX = MeanWrapStepX(SeamlessImage);
    const double WrapY = MeanWrapStepY(SeamlessImage);
    const double WorstColumn = MaxInteriorColumnStep(SeamlessImage);
    const double WorstRow = MaxInteriorRowStep(SeamlessImage);
    TestTrue(*FString::Printf(
        TEXT("the left/right wrap is continuous (wrap step %.2f vs worst interior column step %.2f)"),
        WrapX, WorstColumn), WrapX <= WorstColumn * 1.5 + 1.0);
    TestTrue(*FString::Printf(
        TEXT("the top/bottom wrap is continuous (wrap step %.2f vs worst interior row step %.2f)"),
        WrapY, WorstRow), WrapY <= WorstRow * 1.5 + 1.0);

    // Control that the continuity metric can actually see a seam: the same parameters with
    // seamless:false must measure a materially worse wrap. Without this, a metric that always
    // reads small would pass assertion 1 for the wrong reason.
    const double SeamedWrapX = MeanWrapStepX(SeamedImage);
    TestTrue(*FString::Printf(
        TEXT("the wrap metric detects the non-tiling seam (seamless %.2f vs seamless:false %.2f)"),
        WrapX, SeamedWrapX), SeamedWrapX > WrapX * 2.0);

    // 2. It is still noise. Both of these were exactly 0 on the pre-fix generator: the summed
    //    torus coordinates made the image symmetric about its main diagonal, and periodic at
    //    width/scale pixels - a repeated motif on a grid, which is the reported artifact.
    const double Asymmetric = DiagonallyAsymmetricFraction(SeamlessImage);
    TestTrue(*FString::Printf(
        TEXT("the tiling field is not mirrored about its diagonal (asymmetric fraction %.4f)"),
        Asymmetric), Asymmetric > 0.25);

    const int32 MotifPeriod = Size / static_cast<int32>(Scale);
    const double PeriodBreak = SubTilePeriodBreakFraction(SeamlessImage, MotifPeriod);
    TestTrue(*FString::Printf(
        TEXT("the tiling field does not repeat every %d px (differing fraction %.4f)"),
        MotifPeriod, PeriodBreak), PeriodBreak > 0.25);

    // The scale snap that the lattice wrap requires is reported rather than silently applied.
    bool bSeamlessEcho = false;
    double EffectiveScale = 0.0;
    if (SeamlessCapture.Result.IsValid())
    {
        SeamlessCapture.Result->TryGetBoolField(TEXT("seamless"), bSeamlessEcho);
        SeamlessCapture.Result->TryGetNumberField(TEXT("effectiveScale"), EffectiveScale);
    }
    TestTrue(TEXT("the response reports that the output is seamless"), bSeamlessEcho);
    TestEqual(TEXT("the response reports the whole-cell scale actually used"), EffectiveScale, 4.0);

    return true;
}
