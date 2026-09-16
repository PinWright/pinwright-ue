// Copyright (c) 2026 Alexander Penkin. MIT License.

// PinWright_TextureHandlers.cpp
// Phase 9: Texture Generation & Processing
//
// Implements procedural texture creation, processing, and settings management.

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ParamSpec.h"
#include "Handlers/PackagePathCompose.h"
#include "Handlers/Asset/TextureDumpBuilder.h"
#include "Handlers/Asset/TexturePixelStats.h"
#include "Handlers/Asset/TextureSourceMipLock.h"
#include "Utils/JsonBuilders.h"
#include "PinWrightSubsystem.h"
#include "PinWrightHelpers.h"
#include "Compat/JsonKeyCompat.h"
#include "Utils/AssetUtils.h"
#include "Dom/JsonObject.h"
#include "Math/Float16.h"
#include "Engine/Texture.h"
#include "Engine/Texture2D.h"
#include "TextureResource.h"
#include "TextureCompiler.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "Factories/Texture2dFactoryNew.h"
// UObject/SavePackage.h is not needed - real texture saves route through
// SaveAssetToDiskReportingPresence() (UEditorAssetLibrary::SaveLoadedAsset) in the helpers
#include "Misc/PackageName.h"
#include "HAL/PlatformFileManager.h"
// TextureCompressorModule removed in UE 5.7
#include "Engine/TextureRenderTarget2D.h"
#include "Kismet/KismetRenderingLibrary.h"

// Helper macro for error responses
#define TEXTURE_ERROR_RESPONSE(Msg) \
    Response->SetBoolField(TEXT("success"), false); \
    Response->SetStringField(TEXT("error"), Msg); \
    return Response;

// Use consolidated JSON helpers from PinWrightHelpers.h
// Aliases for backward compatibility with existing code in this file
#define GetNumberFieldTextAuth GetJsonNumberField
#define GetBoolFieldTextAuth GetJsonBoolField
#define GetStringFieldTextAuth GetJsonStringField

// Rec. 709 relative luminance of an 8-bit RGB triple (ITU-R BT.709), returned as a
// 0-255 weighted sum. Shared by the height-map read and the desaturate verb so the
// luminance basis is defined once.
static float Rec709Luma(uint8 R, uint8 G, uint8 B)
{
    return 0.2126f * R + 0.7152f * G + 0.0722f * B;
}

// Persist a just-created or edited texture asset to disk on the save:true path and record
// an honest saved / pendingFlush verdict on the response. The shared mark-dirty helper
// McpSafeAssetSave (Utils/AssetUtils.cpp) only marks the package dirty + notifies the
// registry and returns true WITHOUT writing a .uasset, so every texture save reported
// success while nothing reached disk and the edit vanished on cold restart
// (B-texture-save-no-disk-write). UTexture2D / UTextureRenderTarget2D are non-Blueprint /
// non-SCS, so the bulkdata-corruption vector that forced the deferred mark-dirty on
// Blueprint edits (B-bp-saved-state-corruption-mcp-edits) does not apply — route the real
// write through the shared forced-save + on-disk IFileManager::FileSize probe (gated by
// ShouldTreatAssetSaveAsSuccess), mirroring the accepted material / audio / niagara
// create-save fixes. AssetCreated is kept on the save path (McpSafeAssetSave used to notify
// the registry at every site). bForce=true writes unconditionally: freshly created texture
// packages are not left dirty, so a dirty-only save would report "already saved" and write
// nothing, leaving the asset in memory only (the metasound create lesson).
static void McpSaveTextureToDisk(const TSharedPtr<FJsonObject>& Response, UObject* Texture, bool bSave)
{
    bool bSavedToDisk = false;
    if (bSave && Texture)
    {
        FAssetRegistryModule::AssetCreated(Texture);
        bSavedToDisk = SaveAssetToDiskReportingPresence(Texture, /*bForce=*/true);
    }
    AddAssetSaveReport(Response, bSave, bSavedToDisk);
}

// Helper to create a texture with given dimensions
static UTexture2D* CreateEmptyTexture(const FString& PackagePath, const FString& TextureName, int32 Width, int32 Height, bool bHDR)
{
    // Guard the composition, not the result. TWO engine calls below log at **Fatal** - a
    // verbosity that is not compiled out in any configuration and ends the PROCESS, taking every
    // unsaved package in a shared editor with it: LongPackageNameToFilename
    // (PackageName.cpp:1437-1445) on a path that maps to no mount root, and CreatePackage
    // (UObjectGlobals.cpp:1087-1120) on a name containing "//" or one that resolves to empty. So
    // `name: "a//b"` did not fail this helper, it killed the editor, and the `if (!Package)` check
    // below was never reached (board B-createpackage-unvalidated-paths-plugin-wide).
    // NormalizeContentAssetPath (Utils/PathUtils.h) is run on the FOLDER half first, and it is
    // still not the guard: it collapses an interior "//" and returns EMPTY for a traversing or
    // unmounted root, but it is never applied to the leaf TextureName, and that half alone is
    // enough to kill the process. An empty return is safe here rather than silent - it composes
    // "/<TextureName>", which IsValidLongPackageName refuses with the engine's own reason.
    FString FullPath;
    FString PathError;
    if (!PinWrightComposeAssetPackagePath(NormalizeContentAssetPath(PackagePath), TextureName,
                                          FullPath, PathError))
    {
        // Warning, not Error: this is a refused caller argument, not a plugin fault. The wire
        // response is the caller's existing "Failed to create texture" refusal; the engine's own
        // reason for the refusal is here.
        UE_LOG(LogPinWrightSubsystem, Warning,
            TEXT("CreateEmptyTexture refused '%s' in '%s': %s"),
            *TextureName, *PackagePath, *PathError);
        return nullptr;
    }

    // Create package
    FString PackageFileName = FPackageName::LongPackageNameToFilename(FullPath, FPackageName::GetAssetPackageExtension());
    UPackage* Package = CreatePackage(*FullPath);
    if (!Package)
    {
        return nullptr;
    }
    
    // Create texture
    UTexture2D* NewTexture = NewObject<UTexture2D>(Package, UTexture2D::StaticClass(), FName(*TextureName), RF_Public | RF_Standalone);

    // Deliberately no hand-built platform data: Source below is the only buffer this helper
    // fills, every caller writes through a TextureSourceMip::FScopedMipLock, and UpdateResource() rebuilds the
    // platform mips from Source. Leaving the platform data null is what makes that rebuild
    // unconditional - UTexture::CachePlatformData skips the build when a platform data object
    // already exists whose derived-data key matches what it wants, and a hand-built one carries
    // no key at all, which the DDC2 path reads as "nothing to do" and leaves the hand-built mips
    // in place. The block that used to sit here also sized its PF_FloatRGBA mip at 16 bytes per
    // pixel; the format is 8.
    NewTexture->Source.Init(Width, Height, 1, 1, bHDR ? TSF_RGBA16F : TSF_BGRA8);
    
    // Set properties
    NewTexture->SRGB = !bHDR;
    NewTexture->CompressionSettings = bHDR ? TC_HDR : TC_Default;
    NewTexture->MipGenSettings = TMGS_FromTextureGroup;
    NewTexture->LODGroup = TEXTUREGROUP_World;
    
    NewTexture->UpdateResource();
    Package->MarkPackageDirty();
    
    return NewTexture;
}

// The noise algorithms create_noise_texture can generate. Both are lattice-based, which is
// what makes one tiling mechanism (index wrapping, below) serve them both.
enum class ENoiseTextureAlgorithm : uint8
{
    Perlin,     // Smoothed value noise - the field this verb has always produced
    Worley      // Cellular F1 distance field; "Voronoi" names the same field
};

// The accepted noiseType spellings, in the order the rejection message lists them. Kept
// beside the parser so an added algorithm cannot be implemented without being advertised.
static const TCHAR* const NoiseTextureAlgorithmNames = TEXT("Perlin, Worley, Voronoi");

// Resolves the noiseType parameter, or returns false for a name outside the supported set.
// Returning false rather than falling back to Perlin is the point: an unrecognized algorithm
// name is a caller mistake, and silently substituting one converts it into a fake success.
static bool ParseNoiseTextureAlgorithm(const FString& Name, ENoiseTextureAlgorithm& OutAlgorithm)
{
    if (Name.Equals(TEXT("Perlin"), ESearchCase::IgnoreCase))
    {
        OutAlgorithm = ENoiseTextureAlgorithm::Perlin;
        return true;
    }
    if (Name.Equals(TEXT("Worley"), ESearchCase::IgnoreCase) ||
        Name.Equals(TEXT("Voronoi"), ESearchCase::IgnoreCase))
    {
        OutAlgorithm = ENoiseTextureAlgorithm::Worley;
        return true;
    }
    return false;
}

// Canonical name of a resolved algorithm, echoed on the response so a caller who passed an
// alias ("Voronoi") can see which field was actually generated.
static const TCHAR* NoiseTextureAlgorithmName(ENoiseTextureAlgorithm Algorithm)
{
    return Algorithm == ENoiseTextureAlgorithm::Worley ? TEXT("Worley") : TEXT("Perlin");
}

// Wraps a lattice cell index into [0, Period). Period <= 0 means "do not wrap", which the
// non-tiling path passes. This is the whole tiling mechanism: a lattice-based field repeats
// exactly when the cell indices feeding its hash repeat, so wrapping the indices at Period
// makes the field periodic at Period cells on both axes with no seam and no other structure.
static int32 WrapLatticeIndex(int32 Index, int32 Period)
{
    if (Period <= 0)
    {
        return Index;
    }
    const int32 Wrapped = Index % Period;
    return Wrapped < 0 ? Wrapped + Period : Wrapped;
}

// Integer lattice hash in [-1, 1]. Extracted verbatim from the lambda Noise2D used to carry,
// so the tiling path can hand it wrapped cell indices and the cellular generator can reuse
// the same per-cell randomness.
static float LatticeHash(int32 X, int32 Y, int32 Seed)
{
    int32 N = X + Y * 57 + Seed * 131;
    N = (N << 13) ^ N;
    return (1.0f - ((N * (N * N * 15731 + 789221) + 1376312589) & 0x7fffffff) / 1073741824.0f);
}

// Smoothed value noise: bilinear interpolation of the lattice hash. Period wraps the cell
// indices so the field repeats exactly every Period cells (0 = no wrapping, byte-identical
// to the untiled field this function has always produced).
static float Noise2D(float X, float Y, int32 Seed, int32 Period)
{
    int32 IntX = FMath::FloorToInt(X);
    int32 IntY = FMath::FloorToInt(Y);
    float FracX = X - IntX;
    float FracY = Y - IntY;

    const int32 X0 = WrapLatticeIndex(IntX, Period);
    const int32 X1 = WrapLatticeIndex(IntX + 1, Period);
    const int32 Y0 = WrapLatticeIndex(IntY, Period);
    const int32 Y1 = WrapLatticeIndex(IntY + 1, Period);

    // Bilinear interpolation
    float V00 = LatticeHash(X0, Y0, Seed);
    float V10 = LatticeHash(X1, Y0, Seed);
    float V01 = LatticeHash(X0, Y1, Seed);
    float V11 = LatticeHash(X1, Y1, Seed);

    // Smoothstep
    float SmoothX = FracX * FracX * (3.0f - 2.0f * FracX);
    float SmoothY = FracY * FracY * (3.0f - 2.0f * FracY);

    float I0 = FMath::Lerp(V00, V10, SmoothX);
    float I1 = FMath::Lerp(V01, V11, SmoothX);

    return FMath::Lerp(I0, I1, SmoothY);
}

// Cellular (Worley / Voronoi) F1: the distance from the sample point to the nearest of one
// jittered feature point per lattice cell. A feature point never leaves its own cell, so the
// 3x3 neighbourhood is the standard search window - a nearer point outside it needs a rare
// configuration and costs only a slight overestimate of the distance. Period wraps the cell
// index, tiling on the same mechanism the value noise uses. Returned in [-1, 1] to match
// Noise2D's range, dark at the feature points and bright at the cell boundaries.
static float WorleyF1(float X, float Y, int32 Seed, int32 Period)
{
    const int32 CellX = FMath::FloorToInt(X);
    const int32 CellY = FMath::FloorToInt(Y);

    float NearestSquared = TNumericLimits<float>::Max();
    for (int32 OffsetY = -1; OffsetY <= 1; OffsetY++)
    {
        for (int32 OffsetX = -1; OffsetX <= 1; OffsetX++)
        {
            const int32 NeighborX = CellX + OffsetX;
            const int32 NeighborY = CellY + OffsetY;
            const int32 HashX = WrapLatticeIndex(NeighborX, Period);
            const int32 HashY = WrapLatticeIndex(NeighborY, Period);

            // Two decorrelated hashes of the same cell place its feature point inside it.
            const float JitterX = LatticeHash(HashX, HashY, Seed) * 0.5f + 0.5f;
            const float JitterY = LatticeHash(HashX, HashY, Seed + 8191) * 0.5f + 0.5f;

            const float DX = (static_cast<float>(NeighborX) + JitterX) - X;
            const float DY = (static_cast<float>(NeighborY) + JitterY) - Y;
            NearestSquared = FMath::Min(NearestSquared, DX * DX + DY * DY);
        }
    }

    return FMath::Clamp(FMath::Sqrt(NearestSquared), 0.0f, 1.0f) * 2.0f - 1.0f;
}

// One octave of the selected algorithm.
static float SampleNoiseAlgorithm(ENoiseTextureAlgorithm Algorithm, float X, float Y, int32 Seed, int32 Period)
{
    return Algorithm == ENoiseTextureAlgorithm::Worley
        ? WorleyF1(X, Y, Seed, Period)
        : Noise2D(X, Y, Seed, Period);
}

// The octave counts FBMNoise is defined for. Zero is excluded because the accumulation below
// would never run: MaxValue stays 0 and the final Total / MaxValue is 0/0. That NaN does not
// reach the buffer as a NaN - FMath::Clamp is Max(Min(X, Hi), Lo) and every comparison against
// a NaN is false, so Clamp(NaN, 0, 1) returns 1 on every engine version in range - it reaches
// it as a flat white image reported as a success, which is worse: the caller cannot tell the
// output from a legitimate one. The upper bound is where an octave stops being able to change
// a pixel: at 16 octaves the base frequency has been multiplied by lacunarity fifteen times,
// past any practical texture resolution, and it also bounds a per-pixel loop that a caller can
// otherwise drive to an editor hang with one large number.
static constexpr int32 MinNoiseTextureOctaves = 1;
static constexpr int32 MaxNoiseTextureOctaves = 16;

// The persistence values FBMNoise is defined for, and the second route into the same 0/0 the
// octave range above closes. A negative persistence alternates the sign of the amplitude terms,
// so on an even octave count they cancel to exactly zero and the final Total / MaxValue divides
// by that zero - the resulting NaN is laundered by the same FMath::Clamp into a flat white image
// reported as a success. The upper bound is 1 because the parameter is a falloff: above 1 the
// amplitudes grow with each octave instead of shrinking, and a large enough value overflows the
// float accumulation to infinity by the sixteenth octave, whose inf / inf normalises to the same
// laundered NaN.
static constexpr float MinNoiseTexturePersistence = 0.0f;
static constexpr float MaxNoiseTexturePersistence = 1.0f;

// FBM noise for octaves. BasePeriod is the tile period of the base octave in lattice cells
// (0 = not tiling). Each octave gets its own integer period and has its frequency snapped to
// it, because a lattice only repeats on whole cells: an unsnapped frequency would put the
// wrap point mid-cell and reopen the seam the tiling exists to close. The snap moves an
// octave's frequency by at most half a cell out of BasePeriod * Lacunarity^i.
static float FBMNoise(ENoiseTextureAlgorithm Algorithm, float X, float Y, int32 Octaves,
    float Persistence, float Lacunarity, int32 Seed, int32 BasePeriod)
{
    float Total = 0.0f;
    float Amplitude = 1.0f;
    float Frequency = 1.0f;
    float MaxValue = 0.0f;

    for (int32 i = 0; i < Octaves; i++)
    {
        float OctaveFrequency = Frequency;
        int32 OctavePeriod = 0;
        if (BasePeriod > 0)
        {
            OctavePeriod = FMath::Max(1, FMath::RoundToInt(BasePeriod * Frequency));
            OctaveFrequency = static_cast<float>(OctavePeriod) / static_cast<float>(BasePeriod);
        }
        Total += SampleNoiseAlgorithm(Algorithm, X * OctaveFrequency, Y * OctaveFrequency,
            Seed + i, OctavePeriod) * Amplitude;
        MaxValue += Amplitude;
        Amplitude *= Persistence;
        Frequency *= Lacunarity;
    }

    return Total / MaxValue;
}

static TSharedPtr<FJsonObject> ExecuteTextureAction(const TSharedPtr<FJsonObject>& Params)
{
    TSharedPtr<FJsonObject> Response = MakeShared<FJsonObject>();
    
    FString SubAction = GetStringFieldTextAuth(Params, TEXT("subAction"), TEXT(""));
    
    // ===== PROCEDURAL GENERATION =====
    
    if (SubAction == TEXT("create_noise_texture"))
    {
        // Validate that no unknown/invalid parameters are present
        TSet<FString> ValidParams = {
            TEXT("subAction"), TEXT("name"), TEXT("path"), TEXT("noiseType"),
            TEXT("width"), TEXT("height"), TEXT("scale"), TEXT("octaves"),
            TEXT("persistence"), TEXT("lacunarity"), TEXT("seed"),
            TEXT("seamless"), TEXT("hdr"), TEXT("save")
        };
        for (const auto& Field : Params->Values)
        {
            if (!ValidParams.Contains(EARGCompat::JsonKeyToString(Field.Key)))
            {
                TEXTURE_ERROR_RESPONSE(FString::Printf(TEXT("Invalid parameter: %s"), *Field.Key));
            }
        }

        FString Name = GetStringFieldTextAuth(Params, TEXT("name"), TEXT(""));
        FString Path = GetStringFieldTextAuth(Params, TEXT("path"), TEXT("/Game/Textures"));
        
        // SECURITY: Validate and sanitize path to prevent path traversal attacks
        FString SanitizedPath = SanitizeProjectRelativePath(Path);
        if (SanitizedPath.IsEmpty())
        {
            TEXTURE_ERROR_RESPONSE(TEXT("Invalid path: contains traversal or invalid characters"));
        }
        Path = SanitizedPath;
        
        // Validate name for security
        FString SanitizedName = SanitizeAssetName(Name);
        if (SanitizedName.IsEmpty())
        {
            TEXTURE_ERROR_RESPONSE(TEXT("Invalid name: contains invalid characters"));
        }
        Name = SanitizedName;
        
        FString NoiseType = GetStringFieldTextAuth(Params, TEXT("noiseType"), TEXT("Perlin"));
        int32 Width = static_cast<int32>(GetNumberFieldTextAuth(Params, TEXT("width"), 1024));
        int32 Height = static_cast<int32>(GetNumberFieldTextAuth(Params, TEXT("height"), 1024));
        float Scale = static_cast<float>(GetNumberFieldTextAuth(Params, TEXT("scale"), 1.0));
        int32 Octaves = static_cast<int32>(GetNumberFieldTextAuth(Params, TEXT("octaves"), 4));
        float Persistence = static_cast<float>(GetNumberFieldTextAuth(Params, TEXT("persistence"), 0.5));
        float Lacunarity = static_cast<float>(GetNumberFieldTextAuth(Params, TEXT("lacunarity"), 2.0));
        int32 Seed = static_cast<int32>(GetNumberFieldTextAuth(Params, TEXT("seed"), 0));
        bool bSeamless = GetBoolFieldTextAuth(Params, TEXT("seamless"), false);
        bool bHDR = GetBoolFieldTextAuth(Params, TEXT("hdr"), false);
        bool bSave = GetBoolFieldTextAuth(Params, TEXT("save"), true);
        
        if (Name.IsEmpty())
        {
            TEXTURE_ERROR_RESPONSE(TEXT("Name is required"));
        }

        // Resolve the algorithm before anything is created, so a rejected noiseType leaves no
        // asset behind. noiseType used to be parsed into a local nothing read, which made every
        // value - including a name no algorithm answers to - produce the same field and report
        // success.
        ENoiseTextureAlgorithm Algorithm = ENoiseTextureAlgorithm::Perlin;
        if (!ParseNoiseTextureAlgorithm(NoiseType, Algorithm))
        {
            TEXTURE_ERROR_RESPONSE(FString::Printf(
                TEXT("Unknown noiseType '%s'. Supported: %s"), *NoiseType, NoiseTextureAlgorithmNames));
        }

        // Reject an octave count the FBM is not defined for, before anything is created, and
        // name the range. octaves:0 reads as "no octaves of detail" and used to be accepted:
        // the accumulation ran zero times and normalised by zero, and the resulting NaN was
        // clamped into a flat white texture reported as a success. Substituting 1 for it would
        // put this parameter back in the accepted-and-ignored shape noiseType was just taken
        // out of, so refuse instead.
        if (Octaves < MinNoiseTextureOctaves || Octaves > MaxNoiseTextureOctaves)
        {
            TEXTURE_ERROR_RESPONSE(FString::Printf(
                TEXT("octaves must be between %d and %d, got %d. An FBM with no octaves has no ")
                TEXT("value to normalise by, and beyond %d an octave can no longer change a pixel."),
                MinNoiseTextureOctaves, MaxNoiseTextureOctaves, Octaves, MaxNoiseTextureOctaves));
        }

        // Reject a persistence the FBM cannot normalise, before anything is created, for the same
        // reason the octave range is refused rather than substituted: persistence:-1 with an even
        // octave count cancels the amplitude terms to exactly zero and divides by that zero, and
        // the clamped NaN ships as a flat white texture reported as a success. Written as a
        // negated range test rather than the octaves check's pair of comparisons because every
        // comparison against a NaN is false, so a non-finite value would pass that pair through
        // into the same division.
        if (!(Persistence >= MinNoiseTexturePersistence && Persistence <= MaxNoiseTexturePersistence))
        {
            TEXTURE_ERROR_RESPONSE(FString::Printf(
                TEXT("persistence must be between %g and %g, got %g. A negative persistence makes ")
                TEXT("the octave amplitudes cancel to zero, leaving the FBM nothing to normalise ")
                TEXT("by, and above %g they grow instead of falling off."),
                MinNoiseTexturePersistence, MaxNoiseTexturePersistence, Persistence,
                MaxNoiseTexturePersistence));
        }

        // Create texture
        UTexture2D* NewTexture = CreateEmptyTexture(Path, Name, Width, Height, bHDR);
        if (!NewTexture)
        {
            TEXTURE_ERROR_RESPONSE(TEXT("Failed to create texture"));
        }

        // Lock source data and fill with noise. Scoped: the guard releases on every exit
        // path below, and its ctor flushes the async build CreateEmptyTexture just started —
        // that build holds a read lock on this same source and would otherwise refuse this
        // write lock (Handlers/Asset/TextureSourceMipLock.h).
        TextureSourceMip::FScopedMipLock MipLock(
            NewTexture, TEXT("output"), /*bReadOnly=*/false, TextureSourceMip::EFormatPolicy::Any);
        if (!MipLock.IsValid())
        {
            TEXTURE_ERROR_RESPONSE(MipLock.GetError());
        }
        uint8* MipData = MipLock.GetWriteData();

        // Seamless output tiles by wrapping the noise lattice, which only repeats on whole
        // cells - so the tile spans an integer number of cells and `scale` snaps to that count,
        // reported back as effectiveScale. There is no 4D noise function here to sample a 4D
        // torus with, and the previous attempt to fake one summed its four torus coordinates
        // into the two arguments of a 2D lookup: cos(a)+cos(b) and sin(a)+sin(b) are symmetric
        // and non-injective, so distinct pixels collapsed onto the X+-Y diagonals and the
        // "seamless" flag drew a periodic diagonal lattice instead of noise.
        const int32 TilePeriod = bSeamless ? FMath::Max(1, FMath::RoundToInt(Scale)) : 0;
        const float EffectiveScale = bSeamless ? static_cast<float>(TilePeriod) : Scale;

        // hdr:true initialises the source as TSF_RGBA16F - four FFloat16 channels in R, G, B, A
        // order, eight bytes per pixel, not the four BGRA bytes this loop used to write
        // unconditionally. Writing bytes into it filled only the first half of the buffer and
        // had each BGRA pair reinterpreted as one half-float, so the advertised HDR output was
        // garbage over a half-empty image. Branch on the format the source actually carries
        // rather than on the requested flag, so the fill can never disagree with the allocation.
        const bool bHalfFloatSource = NewTexture->Source.GetFormat() == TSF_RGBA16F;
        const int64 BytesPerPixel = bHalfFloatSource ? 8 : 4;

        for (int32 Y = 0; Y < Height; Y++)
        {
            for (int32 X = 0; X < Width; X++)
            {
                float NX = static_cast<float>(X) / static_cast<float>(Width) * EffectiveScale;
                float NY = static_cast<float>(Y) / static_cast<float>(Height) * EffectiveScale;

                float NoiseValue = FBMNoise(Algorithm, NX, NY, Octaves, Persistence, Lacunarity,
                    Seed, TilePeriod);

                // Normalize to 0-1 range
                NoiseValue = (NoiseValue + 1.0f) * 0.5f;
                NoiseValue = FMath::Clamp(NoiseValue, 0.0f, 1.0f);
                
                const int64 PixelIndex = (static_cast<int64>(Y) * Width + X) * BytesPerPixel;
                if (bHalfFloatSource)
                {
                    // Linear half-floats: SRGB is off on the HDR path, so the normalised
                    // value goes in as-is rather than being encoded into a byte.
                    FFloat16* Pixel = reinterpret_cast<FFloat16*>(MipData + PixelIndex);
                    Pixel[0] = FFloat16(NoiseValue); // R
                    Pixel[1] = FFloat16(NoiseValue); // G
                    Pixel[2] = FFloat16(NoiseValue); // B
                    Pixel[3] = FFloat16(1.0f);       // A
                }
                else
                {
                    // Write pixel data (BGRA8 format)
                    uint8 ByteValue = static_cast<uint8>(NoiseValue * 255.0f);
                    MipData[PixelIndex + 0] = ByteValue; // B
                    MipData[PixelIndex + 1] = ByteValue; // G
                    MipData[PixelIndex + 2] = ByteValue; // R
                    MipData[PixelIndex + 3] = 255;       // A
                }
            }
        }
        
        // Hand the source back before UpdateResource(): the rebuild runs
        // FTextureSource::CheckTextureIsUnlocked and asserts on a still-locked source.
        MipLock.Release();
        NewTexture->UpdateResource();

        McpSaveTextureToDisk(Response, NewTexture, bSave);

Response->SetBoolField(TEXT("success"), true);
        Response->SetStringField(TEXT("message"), FString::Printf(TEXT("Noise texture '%s' created"), *Name));
        // Report what was generated, not what was asked for: the resolved algorithm (so an
        // alias spelling is visible) and, when tiling, the whole-cell scale actually used.
        Response->SetStringField(TEXT("noiseType"), NoiseTextureAlgorithmName(Algorithm));
        Response->SetBoolField(TEXT("seamless"), bSeamless);
        // Read off the source, not off the request: this is the format the pixels were
        // actually written in, which is the thing hdr:true used to be unable to promise.
        Response->SetBoolField(TEXT("hdr"), bHalfFloatSource);
        if (bSeamless)
        {
            Response->SetNumberField(TEXT("effectiveScale"), EffectiveScale);
        }
        AddAssetVerification(Response, NewTexture);
        return Response;
    }

    if (SubAction == TEXT("create_gradient_texture"))
    {
        // Validate that no unknown/invalid parameters are present
        TSet<FString> ValidParams = {
            TEXT("subAction"), TEXT("name"), TEXT("path"), TEXT("gradientType"),
            TEXT("width"), TEXT("height"), TEXT("angle"), TEXT("centerX"),
            TEXT("centerY"), TEXT("radius"), TEXT("hdr"), TEXT("save"),
            TEXT("startColor"), TEXT("endColor")
        };
        for (const auto& Field : Params->Values)
        {
            if (!ValidParams.Contains(EARGCompat::JsonKeyToString(Field.Key)))
            {
                TEXTURE_ERROR_RESPONSE(FString::Printf(TEXT("Invalid parameter: %s"), *Field.Key));
            }
        }

        FString Name = GetStringFieldTextAuth(Params, TEXT("name"), TEXT(""));
        FString Path = GetStringFieldTextAuth(Params, TEXT("path"), TEXT("/Game/Textures"));
        
        // SECURITY: Validate and sanitize path
        FString SanitizedPath = SanitizeProjectRelativePath(Path);
        if (SanitizedPath.IsEmpty())
        {
            TEXTURE_ERROR_RESPONSE(TEXT("Invalid path: contains traversal or invalid characters"));
        }
        Path = SanitizedPath;
        
        // Validate name
        FString SanitizedName = SanitizeAssetName(Name);
        if (SanitizedName.IsEmpty())
        {
            TEXTURE_ERROR_RESPONSE(TEXT("Invalid name: contains invalid characters"));
        }
        Name = SanitizedName;
        
        FString GradientType = GetStringFieldTextAuth(Params, TEXT("gradientType"), TEXT("Linear"));
        int32 Width = static_cast<int32>(GetNumberFieldTextAuth(Params, TEXT("width"), 1024));
        int32 Height = static_cast<int32>(GetNumberFieldTextAuth(Params, TEXT("height"), 1024));
        float Angle = static_cast<float>(GetNumberFieldTextAuth(Params, TEXT("angle"), 0.0));
        float CenterX = static_cast<float>(GetNumberFieldTextAuth(Params, TEXT("centerX"), 0.5));
        float CenterY = static_cast<float>(GetNumberFieldTextAuth(Params, TEXT("centerY"), 0.5));
        float Radius = static_cast<float>(GetNumberFieldTextAuth(Params, TEXT("radius"), 0.5));
        bool bHDR = GetBoolFieldTextAuth(Params, TEXT("hdr"), false);
        bool bSave = GetBoolFieldTextAuth(Params, TEXT("save"), true);
        
        // Get colors
        FLinearColor StartColor(0, 0, 0, 1);
        FLinearColor EndColor(1, 1, 1, 1);
        
        if (Params->HasField(TEXT("startColor")))
        {
            const TSharedPtr<FJsonObject>* StartColorObj;
            if (Params->TryGetObjectField(TEXT("startColor"), StartColorObj))
            {
                StartColor.R = static_cast<float>(GetNumberFieldTextAuth(*StartColorObj, TEXT("r"), 0.0));
                StartColor.G = static_cast<float>(GetNumberFieldTextAuth(*StartColorObj, TEXT("g"), 0.0));
                StartColor.B = static_cast<float>(GetNumberFieldTextAuth(*StartColorObj, TEXT("b"), 0.0));
                StartColor.A = static_cast<float>(GetNumberFieldTextAuth(*StartColorObj, TEXT("a"), 1.0));
            }
        }
        
        if (Params->HasField(TEXT("endColor")))
        {
            const TSharedPtr<FJsonObject>* EndColorObj;
            if (Params->TryGetObjectField(TEXT("endColor"), EndColorObj))
            {
                EndColor.R = static_cast<float>(GetNumberFieldTextAuth(*EndColorObj, TEXT("r"), 1.0));
                EndColor.G = static_cast<float>(GetNumberFieldTextAuth(*EndColorObj, TEXT("g"), 1.0));
                EndColor.B = static_cast<float>(GetNumberFieldTextAuth(*EndColorObj, TEXT("b"), 1.0));
                EndColor.A = static_cast<float>(GetNumberFieldTextAuth(*EndColorObj, TEXT("a"), 1.0));
            }
        }
        
        if (Name.IsEmpty())
        {
            TEXTURE_ERROR_RESPONSE(TEXT("Name is required"));
        }
        
        UTexture2D* NewTexture = CreateEmptyTexture(Path, Name, Width, Height, bHDR);
        if (!NewTexture)
        {
            TEXTURE_ERROR_RESPONSE(TEXT("Failed to create texture"));
        }
        
        TextureSourceMip::FScopedMipLock MipLock(
            NewTexture, TEXT("output"), /*bReadOnly=*/false, TextureSourceMip::EFormatPolicy::Any);
        if (!MipLock.IsValid())
        {
            TEXTURE_ERROR_RESPONSE(MipLock.GetError());
        }
        uint8* MipData = MipLock.GetWriteData();

        // Convert angle to radians for linear gradient
        float AngleRad = FMath::DegreesToRadians(Angle);
        FVector2D GradientDir(FMath::Cos(AngleRad), FMath::Sin(AngleRad));

        // hdr:true initialises the source as TSF_RGBA16F - four FFloat16 channels in R, G, B, A
        // order, eight bytes per pixel, not the four BGRA bytes this loop used to write
        // unconditionally. Writing bytes into it filled only the first half of the buffer and
        // had each BGRA pair reinterpreted as one half-float, so the advertised HDR gradient was
        // garbage over a half-empty image. Branch on the format the source actually carries
        // rather than on the requested flag, so the fill can never disagree with the allocation.
        const bool bHalfFloatSource = NewTexture->Source.GetFormat() == TSF_RGBA16F;
        const int64 BytesPerPixel = bHalfFloatSource ? 8 : 4;

        for (int32 Y = 0; Y < Height; Y++)
        {
            for (int32 X = 0; X < Width; X++)
            {
                float NX = static_cast<float>(X) / static_cast<float>(Width);
                float NY = static_cast<float>(Y) / static_cast<float>(Height);
                
                float T = 0.0f;
                
                if (GradientType == TEXT("Linear"))
                {
                    // Project onto gradient direction
                    T = NX * GradientDir.X + NY * GradientDir.Y;
                    T = FMath::Clamp(T, 0.0f, 1.0f);
                }
                else if (GradientType == TEXT("Radial"))
                {
                    float DX = NX - CenterX;
                    float DY = NY - CenterY;
                    float Dist = FMath::Sqrt(DX * DX + DY * DY);
                    T = FMath::Clamp(Dist / Radius, 0.0f, 1.0f);
                }
                else if (GradientType == TEXT("Angular"))
                {
                    float DX = NX - CenterX;
                    float DY = NY - CenterY;
                    float AngleVal = FMath::Atan2(DY, DX);
                    T = (AngleVal + PI) / (2.0f * PI);
                    T = FMath::Clamp(T, 0.0f, 1.0f);
                }
                
                // Interpolate color
                FLinearColor Color = FMath::Lerp(StartColor, EndColor, T);
                
                // Write pixel
                const int64 PixelIndex = (static_cast<int64>(Y) * Width + X) * BytesPerPixel;
                if (bHalfFloatSource)
                {
                    // Linear half-floats: SRGB is off on the HDR path, so the interpolated
                    // linear colour goes in as-is rather than being encoded into a byte.
                    FFloat16* Pixel = reinterpret_cast<FFloat16*>(MipData + PixelIndex);
                    Pixel[0] = FFloat16(Color.R); // R
                    Pixel[1] = FFloat16(Color.G); // G
                    Pixel[2] = FFloat16(Color.B); // B
                    Pixel[3] = FFloat16(Color.A); // A
                }
                else
                {
                    MipData[PixelIndex + 0] = static_cast<uint8>(Color.B * 255.0f); // B
                    MipData[PixelIndex + 1] = static_cast<uint8>(Color.G * 255.0f); // G
                    MipData[PixelIndex + 2] = static_cast<uint8>(Color.R * 255.0f); // R
                    MipData[PixelIndex + 3] = static_cast<uint8>(Color.A * 255.0f); // A
                }
            }
        }
        
        MipLock.Release();
        NewTexture->UpdateResource();

        McpSaveTextureToDisk(Response, NewTexture, bSave);

Response->SetBoolField(TEXT("success"), true);
        Response->SetStringField(TEXT("message"), FString::Printf(TEXT("Gradient texture '%s' created"), *Name));
        AddAssetVerification(Response, NewTexture);
        return Response;
    }
    
    if (SubAction == TEXT("create_pattern_texture"))
    {
        // Validate that no unknown/invalid parameters are present
        TSet<FString> ValidParams = {
            TEXT("subAction"), TEXT("name"), TEXT("path"), TEXT("patternType"),
            TEXT("width"), TEXT("height"), TEXT("tilesX"), TEXT("tilesY"),
            TEXT("lineWidth"), TEXT("brickRatio"), TEXT("offset"), TEXT("save"),
            TEXT("primaryColor"), TEXT("secondaryColor")
        };
        for (const auto& Field : Params->Values)
        {
            if (!ValidParams.Contains(EARGCompat::JsonKeyToString(Field.Key)))
            {
                TEXTURE_ERROR_RESPONSE(FString::Printf(TEXT("Invalid parameter: %s"), *Field.Key));
            }
        }

        FString Name = GetStringFieldTextAuth(Params, TEXT("name"), TEXT(""));
        FString Path = GetStringFieldTextAuth(Params, TEXT("path"), TEXT("/Game/Textures"));
        
        // SECURITY: Validate and sanitize path
        FString SanitizedPath = SanitizeProjectRelativePath(Path);
        if (SanitizedPath.IsEmpty())
        {
            TEXTURE_ERROR_RESPONSE(TEXT("Invalid path: contains traversal or invalid characters"));
        }
        Path = SanitizedPath;
        
        // Validate name
        FString SanitizedName = SanitizeAssetName(Name);
        if (SanitizedName.IsEmpty())
        {
            TEXTURE_ERROR_RESPONSE(TEXT("Invalid name: contains invalid characters"));
        }
        Name = SanitizedName;
        
        FString PatternType = GetStringFieldTextAuth(Params, TEXT("patternType"), TEXT("Checker"));
        int32 Width = static_cast<int32>(GetNumberFieldTextAuth(Params, TEXT("width"), 1024));
        int32 Height = static_cast<int32>(GetNumberFieldTextAuth(Params, TEXT("height"), 1024));
        int32 TilesX = static_cast<int32>(GetNumberFieldTextAuth(Params, TEXT("tilesX"), 8));
        int32 TilesY = static_cast<int32>(GetNumberFieldTextAuth(Params, TEXT("tilesY"), 8));
        float LineWidth = static_cast<float>(GetNumberFieldTextAuth(Params, TEXT("lineWidth"), 0.02));
        float BrickRatio = static_cast<float>(GetNumberFieldTextAuth(Params, TEXT("brickRatio"), 2.0));
        float Offset = static_cast<float>(GetNumberFieldTextAuth(Params, TEXT("offset"), 0.5));
        bool bSave = GetBoolFieldTextAuth(Params, TEXT("save"), true);
        
        // Get colors
        FLinearColor PrimaryColor(1, 1, 1, 1);
        FLinearColor SecondaryColor(0, 0, 0, 1);
        
        if (Params->HasField(TEXT("primaryColor")))
        {
            const TSharedPtr<FJsonObject>* ColorObj;
            if (Params->TryGetObjectField(TEXT("primaryColor"), ColorObj))
            {
                PrimaryColor.R = static_cast<float>(GetNumberFieldTextAuth(*ColorObj, TEXT("r"), 1.0));
                PrimaryColor.G = static_cast<float>(GetNumberFieldTextAuth(*ColorObj, TEXT("g"), 1.0));
                PrimaryColor.B = static_cast<float>(GetNumberFieldTextAuth(*ColorObj, TEXT("b"), 1.0));
                PrimaryColor.A = static_cast<float>(GetNumberFieldTextAuth(*ColorObj, TEXT("a"), 1.0));
            }
        }
        
        if (Params->HasField(TEXT("secondaryColor")))
        {
            const TSharedPtr<FJsonObject>* ColorObj;
            if (Params->TryGetObjectField(TEXT("secondaryColor"), ColorObj))
            {
                SecondaryColor.R = static_cast<float>(GetNumberFieldTextAuth(*ColorObj, TEXT("r"), 0.0));
                SecondaryColor.G = static_cast<float>(GetNumberFieldTextAuth(*ColorObj, TEXT("g"), 0.0));
                SecondaryColor.B = static_cast<float>(GetNumberFieldTextAuth(*ColorObj, TEXT("b"), 0.0));
                SecondaryColor.A = static_cast<float>(GetNumberFieldTextAuth(*ColorObj, TEXT("a"), 1.0));
            }
        }
        
        if (Name.IsEmpty())
        {
            TEXTURE_ERROR_RESPONSE(TEXT("Name is required"));
        }
        
        UTexture2D* NewTexture = CreateEmptyTexture(Path, Name, Width, Height, false);
        if (!NewTexture)
        {
            TEXTURE_ERROR_RESPONSE(TEXT("Failed to create texture"));
        }
        
        TextureSourceMip::FScopedMipLock MipLock(
            NewTexture, TEXT("output"), /*bReadOnly=*/false, TextureSourceMip::EFormatPolicy::Any);
        if (!MipLock.IsValid())
        {
            TEXTURE_ERROR_RESPONSE(MipLock.GetError());
        }
        uint8* MipData = MipLock.GetWriteData();

        for (int32 Y = 0; Y < Height; Y++)
        {
            for (int32 X = 0; X < Width; X++)
            {
                float NX = static_cast<float>(X) / static_cast<float>(Width);
                float NY = static_cast<float>(Y) / static_cast<float>(Height);
                
                bool bUsePrimary = true;
                
                if (PatternType == TEXT("Checker"))
                {
                    int32 CellX = static_cast<int32>(NX * TilesX);
                    int32 CellY = static_cast<int32>(NY * TilesY);
                    bUsePrimary = ((CellX + CellY) % 2) == 0;
                }
                else if (PatternType == TEXT("Grid"))
                {
                    float CellWidth = 1.0f / TilesX;
                    float CellHeight = 1.0f / TilesY;
                    float LocalX = FMath::Fmod(NX, CellWidth) / CellWidth;
                    float LocalY = FMath::Fmod(NY, CellHeight) / CellHeight;
                    bUsePrimary = (LocalX > LineWidth && LocalX < (1.0f - LineWidth) &&
                                   LocalY > LineWidth && LocalY < (1.0f - LineWidth));
                }
                else if (PatternType == TEXT("Brick"))
                {
                    float BrickHeight = 1.0f / TilesY;
                    int32 Row = static_cast<int32>(NY * TilesY);
                    float RowOffset = (Row % 2 == 1) ? Offset / TilesX : 0.0f;
                    float AdjustedX = FMath::Fmod(NX + RowOffset, 1.0f);
                    
                    float BrickWidth = BrickRatio / TilesX;
                    float LocalX = FMath::Fmod(AdjustedX, BrickWidth) / BrickWidth;
                    float LocalY = FMath::Fmod(NY, BrickHeight) / BrickHeight;
                    
                    bUsePrimary = (LocalX > LineWidth && LocalX < (1.0f - LineWidth) &&
                                   LocalY > LineWidth && LocalY < (1.0f - LineWidth));
                }
                else if (PatternType == TEXT("Stripes"))
                {
                    int32 StripeIndex = static_cast<int32>(NX * TilesX);
                    bUsePrimary = (StripeIndex % 2) == 0;
                }
                else if (PatternType == TEXT("Dots"))
                {
                    float CellWidth = 1.0f / TilesX;
                    float CellHeight = 1.0f / TilesY;
                    float CenterLocalX = FMath::Fmod(NX, CellWidth) / CellWidth - 0.5f;
                    float CenterLocalY = FMath::Fmod(NY, CellHeight) / CellHeight - 0.5f;
                    float Dist = FMath::Sqrt(CenterLocalX * CenterLocalX + CenterLocalY * CenterLocalY);
                    bUsePrimary = Dist < 0.3f;
                }
                
                FLinearColor Color = bUsePrimary ? PrimaryColor : SecondaryColor;
                
                int32 PixelIndex = (Y * Width + X) * 4;
                MipData[PixelIndex + 0] = static_cast<uint8>(Color.B * 255.0f);
                MipData[PixelIndex + 1] = static_cast<uint8>(Color.G * 255.0f);
                MipData[PixelIndex + 2] = static_cast<uint8>(Color.R * 255.0f);
                MipData[PixelIndex + 3] = static_cast<uint8>(Color.A * 255.0f);
            }
        }
        
        MipLock.Release();
        NewTexture->UpdateResource();

        McpSaveTextureToDisk(Response, NewTexture, bSave);

Response->SetBoolField(TEXT("success"), true);
        Response->SetStringField(TEXT("message"), FString::Printf(TEXT("Pattern texture '%s' created"), *Name));
        AddAssetVerification(Response, NewTexture);
        return Response;
    }
    
    if (SubAction == TEXT("create_normal_from_height"))
    {
        // Validate that no unknown/invalid parameters are present
        TSet<FString> ValidParams = {
            TEXT("subAction"), TEXT("sourceTexture"), TEXT("name"), TEXT("path"),
            TEXT("strength"), TEXT("algorithm"), TEXT("flipY"), TEXT("save"),
            TEXT("channelMode")
        };
        for (const auto& Field : Params->Values)
        {
            if (!ValidParams.Contains(EARGCompat::JsonKeyToString(Field.Key)))
            {
                TEXTURE_ERROR_RESPONSE(FString::Printf(TEXT("Invalid parameter: %s"), *Field.Key));
            }
        }

        FString SourceTexture = GetStringFieldTextAuth(Params, TEXT("sourceTexture"), TEXT(""));
        FString Name = GetStringFieldTextAuth(Params, TEXT("name"), TEXT(""));
        FString Path = GetStringFieldTextAuth(Params, TEXT("path"), TEXT(""));
        
        // SECURITY: Validate sourceTexture path
        FString SanitizedSource = SanitizeProjectRelativePath(SourceTexture);
        if (SanitizedSource.IsEmpty())
        {
            TEXTURE_ERROR_RESPONSE(TEXT("Invalid sourceTexture: contains traversal or invalid characters"));
        }
        SourceTexture = SanitizedSource;
        
        float Strength = static_cast<float>(GetNumberFieldTextAuth(Params, TEXT("strength"), 1.0));
        FString Algorithm = GetStringFieldTextAuth(Params, TEXT("algorithm"), TEXT("Sobel"));
        bool bFlipY = GetBoolFieldTextAuth(Params, TEXT("flipY"), false);
        bool bSave = GetBoolFieldTextAuth(Params, TEXT("save"), true);
        
        if (SourceTexture.IsEmpty())
        {
            TEXTURE_ERROR_RESPONSE(TEXT("sourceTexture is required"));
        }
        
        // Load source texture
        UTexture2D* HeightMap = Cast<UTexture2D>(StaticLoadObject(UTexture2D::StaticClass(), nullptr, *SourceTexture));
        if (!HeightMap)
        {
            TEXTURE_ERROR_RESPONSE(FString::Printf(TEXT("Failed to load height map: %s"), *SourceTexture));
        }
        
        // Read the height data from the editor source (FTextureSource), which is always
        // CPU-resident and uncompressed — NOT the built platform mip, whose BulkData is
        // GPU-only for a freshly built/compressed source and null-derefs (board
        // B-texture-normal-from-height-crash). Only byte-per-channel source layouts the
        // pixel loop below can interpret are accepted; compressed/float formats would
        // misread the byte order and produce a garbage normal map.
        if (!HeightMap->Source.IsValid())
        {
            TEXTURE_ERROR_RESPONSE(FString::Printf(
                TEXT("Source height map '%s' has no editor source data to read"), *SourceTexture));
        }
        const ETextureSourceFormat SrcFormat = HeightMap->Source.GetFormat();
        if (!TexturePixelStats::IsReadableSourceFormat(SrcFormat))
        {
            TEXTURE_ERROR_RESPONSE(FString::Printf(
                TEXT("Source height map format %s is unsupported; supply an 8-bit BGRA or grayscale (G8) height texture"),
                *JsonBuilders::EnumValueToString(SrcFormat)));
        }

        // Dimensions come from the source mip we actually read.
        int32 Width = HeightMap->Source.GetSizeX();
        int32 Height = HeightMap->Source.GetSizeY();
        
        // Generate output name and path if not specified
        if (Name.IsEmpty())
        {
            Name = FPaths::GetBaseFilename(SourceTexture) + TEXT("_N");
        }
        if (Path.IsEmpty())
        {
            Path = FPaths::GetPath(SourceTexture);
        }
        
        // SECURITY: Validate output path
        FString SanitizedPath = SanitizeProjectRelativePath(Path);
        if (SanitizedPath.IsEmpty())
        {
            TEXTURE_ERROR_RESPONSE(TEXT("Invalid path: contains traversal or invalid characters"));
        }
        Path = SanitizedPath;
        
        // Validate name
        FString SanitizedName = SanitizeAssetName(Name);
        if (SanitizedName.IsEmpty())
        {
            TEXTURE_ERROR_RESPONSE(TEXT("Invalid name: contains invalid characters"));
        }
        Name = SanitizedName;
        
        // Create output texture
        UTexture2D* NormalMap = CreateEmptyTexture(Path, Name, Width, Height, false);
        if (!NormalMap)
        {
            TEXTURE_ERROR_RESPONSE(TEXT("Failed to create normal map texture"));
        }

        // CreateEmptyTexture already kicked off an async DDC build with the default color
        // settings (SRGB=true). We are about to flip SRGB->false / TC_Normalmap and rebuild;
        // if that in-flight build is still running when SRGB changes, its worker re-reads the
        // now-Linear gamma while its snapshot still says sRGB and asserts
        // (MipView.GammaSpace == LayerData.SourceGammaSpace, TextureDerivedDataTask.cpp) —
        // a background-thread crash that takes down the whole editor
        // (board B-texture-normal-from-height-crash). Flush that first build to completion
        // before mutating the gamma-affecting flags so the two builds never overlap.
        FTextureCompilingManager::Get().FinishCompilation({ NormalMap });

        // Set normal map properties
        NormalMap->SRGB = false;
        NormalMap->CompressionSettings = TC_Normalmap;
        
        // Read height data with proper luminance or channel selection
        TArray<float> HeightData;
        HeightData.SetNum(Width * Height);
        
        // Get channel mapping option - defaults to "luminance" for proper grayscale conversion
        // Options: "luminance", "red", "green", "blue", "alpha", "average"
        FString ChannelMode = GetStringFieldTextAuth(Params, TEXT("channelMode"), TEXT("luminance"));
        
        // Lock the editor source mip for reading (always CPU-resident, never the streamed
        // platform mip). SrcFormat is guaranteed BGRA8 or G8 by the guard above.
        TextureSourceMip::FScopedMipLock HeightLock(
            HeightMap, TEXT("source height map"), /*bReadOnly=*/true,
            TextureSourceMip::EFormatPolicy::Bgra8OrG8);
        if (!HeightLock.IsValid())
        {
            TEXTURE_ERROR_RESPONSE(HeightLock.GetError());
        }
        const uint8* HeightPixels = HeightLock.GetReadData();
        const bool bSourceGrayscale = (SrcFormat == TSF_G8);

        for (int32 i = 0; i < Width * Height; i++)
        {
            // BGRA8 source: index 0=B, 1=G, 2=R, 3=A. G8 source: one byte per pixel,
            // replicated to all channels so the channelMode selection stays valid.
            uint8 B, G, R, A;
            if (bSourceGrayscale)
            {
                B = G = R = HeightPixels[i];
                A = 255;
            }
            else
            {
                B = HeightPixels[i * 4 + 0];
                G = HeightPixels[i * 4 + 1];
                R = HeightPixels[i * 4 + 2];
                A = HeightPixels[i * 4 + 3];
            }
            
            float HeightValue;
            if (ChannelMode.Equals(TEXT("red"), ESearchCase::IgnoreCase))
            {
                HeightValue = static_cast<float>(R) / 255.0f;
            }
            else if (ChannelMode.Equals(TEXT("green"), ESearchCase::IgnoreCase))
            {
                HeightValue = static_cast<float>(G) / 255.0f;
            }
            else if (ChannelMode.Equals(TEXT("blue"), ESearchCase::IgnoreCase))
            {
                HeightValue = static_cast<float>(B) / 255.0f;
            }
            else if (ChannelMode.Equals(TEXT("alpha"), ESearchCase::IgnoreCase))
            {
                HeightValue = static_cast<float>(A) / 255.0f;
            }
            else if (ChannelMode.Equals(TEXT("average"), ESearchCase::IgnoreCase))
            {
                HeightValue = (static_cast<float>(R) + static_cast<float>(G) + static_cast<float>(B)) / (255.0f * 3.0f);
            }
            else // Default: Rec. 709 luminance coefficients for proper grayscale
            {
                HeightValue = Rec709Luma(R, G, B) / 255.0f;
            }
            
            HeightData[i] = HeightValue;
        }
        HeightLock.Release();

        // Generate normal map
        TextureSourceMip::FScopedMipLock NormalLock(
            NormalMap, TEXT("output normal map"), /*bReadOnly=*/false,
            TextureSourceMip::EFormatPolicy::Any);
        if (!NormalLock.IsValid())
        {
            TEXTURE_ERROR_RESPONSE(NormalLock.GetError());
        }
        uint8* NormalData = NormalLock.GetWriteData();

        for (int32 Y = 0; Y < Height; Y++)
        {
            for (int32 X = 0; X < Width; X++)
            {
                // Sample neighboring heights with wrap
                auto SampleHeight = [&](int32 SX, int32 SY) -> float {
                    SX = (SX + Width) % Width;
                    SY = (SY + Height) % Height;
                    return HeightData[SY * Width + SX];
                };
                
                float DX, DY;
                
                if (Algorithm == TEXT("Sobel"))
                {
                    // Sobel operator
                    DX = (SampleHeight(X - 1, Y - 1) * -1.0f + SampleHeight(X - 1, Y) * -2.0f + SampleHeight(X - 1, Y + 1) * -1.0f +
                          SampleHeight(X + 1, Y - 1) * 1.0f + SampleHeight(X + 1, Y) * 2.0f + SampleHeight(X + 1, Y + 1) * 1.0f);
                    DY = (SampleHeight(X - 1, Y - 1) * -1.0f + SampleHeight(X, Y - 1) * -2.0f + SampleHeight(X + 1, Y - 1) * -1.0f +
                          SampleHeight(X - 1, Y + 1) * 1.0f + SampleHeight(X, Y + 1) * 2.0f + SampleHeight(X + 1, Y + 1) * 1.0f);
                }
                else
                {
                    // Simple finite difference
                    DX = SampleHeight(X + 1, Y) - SampleHeight(X - 1, Y);
                    DY = SampleHeight(X, Y + 1) - SampleHeight(X, Y - 1);
                }
                
                // Apply strength
                DX *= Strength;
                DY *= Strength;
                
                // Flip Y if needed (DirectX vs OpenGL)
                if (bFlipY)
                {
                    DY = -DY;
                }
                
                // Create normal vector
                FVector Normal(-DX, -DY, 1.0f);
                Normal.Normalize();
                
                // Convert to 0-1 range
                int32 PixelIndex = (Y * Width + X) * 4;
                NormalData[PixelIndex + 0] = static_cast<uint8>((Normal.Z * 0.5f + 0.5f) * 255.0f); // B = Z
                NormalData[PixelIndex + 1] = static_cast<uint8>((Normal.Y * 0.5f + 0.5f) * 255.0f); // G = Y
                NormalData[PixelIndex + 2] = static_cast<uint8>((Normal.X * 0.5f + 0.5f) * 255.0f); // R = X
                NormalData[PixelIndex + 3] = 255;
            }
        }
        
        NormalLock.Release();
        NormalMap->UpdateResource();
        
        McpSaveTextureToDisk(Response, NormalMap, bSave);
        
Response->SetBoolField(TEXT("success"), true);
        Response->SetStringField(TEXT("message"), TEXT("Normal map created from height map"));
        AddAssetVerification(Response, NormalMap);
        return Response;
    }
    
    // ===== TEXTURE SETTINGS =====
    
    if (SubAction == TEXT("set_compression_settings"))
    {
        // Validate that no unknown/invalid parameters are present
        TSet<FString> ValidParams = {
            TEXT("subAction"), TEXT("assetPath"), TEXT("compressionSettings"), TEXT("save")
        };
        for (const auto& Field : Params->Values)
        {
            if (!ValidParams.Contains(EARGCompat::JsonKeyToString(Field.Key)))
            {
                TEXTURE_ERROR_RESPONSE(FString::Printf(TEXT("Invalid parameter: %s"), *Field.Key));
            }
        }

        FString AssetPath = GetStringFieldTextAuth(Params, TEXT("assetPath"), TEXT(""));
        
        // SECURITY: Validate assetPath
        FString SanitizedAssetPath = SanitizeProjectRelativePath(AssetPath);
        if (SanitizedAssetPath.IsEmpty())
        {
            TEXTURE_ERROR_RESPONSE(TEXT("Invalid assetPath: contains traversal or invalid characters"));
        }
        AssetPath = SanitizedAssetPath;
        
        FString CompressionSettingsStr = GetStringFieldTextAuth(Params, TEXT("compressionSettings"), TEXT("TC_Default"));
        bool bSave = GetBoolFieldTextAuth(Params, TEXT("save"), true);
        
        if (AssetPath.IsEmpty())
        {
            TEXTURE_ERROR_RESPONSE(TEXT("assetPath is required"));
        }
        
        UTexture2D* Texture = Cast<UTexture2D>(StaticLoadObject(UTexture2D::StaticClass(), nullptr, *AssetPath));
        if (!Texture)
        {
            TEXTURE_ERROR_RESPONSE(FString::Printf(TEXT("Failed to load texture: %s"), *AssetPath));
        }
        
        // Map string to enum
        TextureCompressionSettings NewSetting = TC_Default;
        if (CompressionSettingsStr == TEXT("TC_Normalmap")) NewSetting = TC_Normalmap;
        else if (CompressionSettingsStr == TEXT("TC_Masks")) NewSetting = TC_Masks;
        else if (CompressionSettingsStr == TEXT("TC_Grayscale")) NewSetting = TC_Grayscale;
        else if (CompressionSettingsStr == TEXT("TC_Displacementmap")) NewSetting = TC_Displacementmap;
        else if (CompressionSettingsStr == TEXT("TC_VectorDisplacementmap")) NewSetting = TC_VectorDisplacementmap;
        else if (CompressionSettingsStr == TEXT("TC_HDR")) NewSetting = TC_HDR;
        else if (CompressionSettingsStr == TEXT("TC_EditorIcon")) NewSetting = TC_EditorIcon;
        else if (CompressionSettingsStr == TEXT("TC_Alpha")) NewSetting = TC_Alpha;
        else if (CompressionSettingsStr == TEXT("TC_DistanceFieldFont")) NewSetting = TC_DistanceFieldFont;
        else if (CompressionSettingsStr == TEXT("TC_HDR_Compressed")) NewSetting = TC_HDR_Compressed;
        else if (CompressionSettingsStr == TEXT("TC_BC7")) NewSetting = TC_BC7;
        
        Texture->CompressionSettings = NewSetting;
        Texture->UpdateResource();
        Texture->MarkPackageDirty();
        
        McpSaveTextureToDisk(Response, Texture, bSave);
        
Response->SetBoolField(TEXT("success"), true);
        Response->SetStringField(TEXT("message"), FString::Printf(TEXT("Compression set to %s"), *CompressionSettingsStr));
        AddAssetVerification(Response, Texture);
        return Response;
    }
    
    if (SubAction == TEXT("set_texture_group"))
    {
        // Validate that no unknown/invalid parameters are present
        TSet<FString> ValidParams = {
            TEXT("subAction"), TEXT("assetPath"), TEXT("textureGroup"), TEXT("save")
        };
        for (const auto& Field : Params->Values)
        {
            if (!ValidParams.Contains(EARGCompat::JsonKeyToString(Field.Key)))
            {
                TEXTURE_ERROR_RESPONSE(FString::Printf(TEXT("Invalid parameter: %s"), *Field.Key));
            }
        }

        FString AssetPath = GetStringFieldTextAuth(Params, TEXT("assetPath"), TEXT(""));
        
        // SECURITY: Validate assetPath
        FString SanitizedAssetPath = SanitizeProjectRelativePath(AssetPath);
        if (SanitizedAssetPath.IsEmpty())
        {
            TEXTURE_ERROR_RESPONSE(TEXT("Invalid assetPath: contains traversal or invalid characters"));
        }
        AssetPath = SanitizedAssetPath;
        
        FString TextureGroup = GetStringFieldTextAuth(Params, TEXT("textureGroup"), TEXT("TEXTUREGROUP_World"));
        bool bSave = GetBoolFieldTextAuth(Params, TEXT("save"), true);
        
        if (AssetPath.IsEmpty())
        {
            TEXTURE_ERROR_RESPONSE(TEXT("assetPath is required"));
        }
        
        UTexture2D* Texture = Cast<UTexture2D>(StaticLoadObject(UTexture2D::StaticClass(), nullptr, *AssetPath));
        if (!Texture)
        {
            TEXTURE_ERROR_RESPONSE(FString::Printf(TEXT("Failed to load texture: %s"), *AssetPath));
        }
        
        // Map common texture groups
        ::TextureGroup NewGroup = TEXTUREGROUP_World;
        if (TextureGroup.Contains(TEXT("Character"))) NewGroup = TEXTUREGROUP_Character;
        else if (TextureGroup.Contains(TEXT("Weapon"))) NewGroup = TEXTUREGROUP_Weapon;
        else if (TextureGroup.Contains(TEXT("Vehicle"))) NewGroup = TEXTUREGROUP_Vehicle;
        else if (TextureGroup.Contains(TEXT("Cinematic"))) NewGroup = TEXTUREGROUP_Cinematic;
        else if (TextureGroup.Contains(TEXT("Effects"))) NewGroup = TEXTUREGROUP_Effects;
        else if (TextureGroup.Contains(TEXT("Skybox"))) NewGroup = TEXTUREGROUP_Skybox;
        else if (TextureGroup.Contains(TEXT("UI"))) NewGroup = TEXTUREGROUP_UI;
        else if (TextureGroup.Contains(TEXT("Lightmap"))) NewGroup = TEXTUREGROUP_Lightmap;
        else if (TextureGroup.Contains(TEXT("RenderTarget"))) NewGroup = TEXTUREGROUP_RenderTarget;
        else if (TextureGroup.Contains(TEXT("Bokeh"))) NewGroup = TEXTUREGROUP_Bokeh;
        else if (TextureGroup.Contains(TEXT("Pixels2D"))) NewGroup = TEXTUREGROUP_Pixels2D;
        
        Texture->LODGroup = NewGroup;
        Texture->UpdateResource();
        Texture->MarkPackageDirty();
        
        McpSaveTextureToDisk(Response, Texture, bSave);
        
Response->SetBoolField(TEXT("success"), true);
        Response->SetStringField(TEXT("message"), FString::Printf(TEXT("Texture group set to %s"), *TextureGroup));
        AddAssetVerification(Response, Texture);
        return Response;
    }
    
    if (SubAction == TEXT("set_lod_bias"))
    {
        // Validate that no unknown/invalid parameters are present
        TSet<FString> ValidParams = {
            TEXT("subAction"), TEXT("assetPath"), TEXT("lodBias"), TEXT("save")
        };
        for (const auto& Field : Params->Values)
        {
            if (!ValidParams.Contains(EARGCompat::JsonKeyToString(Field.Key)))
            {
                TEXTURE_ERROR_RESPONSE(FString::Printf(TEXT("Invalid parameter: %s"), *Field.Key));
            }
        }

        FString AssetPath = GetStringFieldTextAuth(Params, TEXT("assetPath"), TEXT(""));
        
        // SECURITY: Validate assetPath
        FString SanitizedAssetPath = SanitizeProjectRelativePath(AssetPath);
        if (SanitizedAssetPath.IsEmpty())
        {
            TEXTURE_ERROR_RESPONSE(TEXT("Invalid assetPath: contains traversal or invalid characters"));
        }
        AssetPath = SanitizedAssetPath;
        
        int32 LODBias = static_cast<int32>(GetNumberFieldTextAuth(Params, TEXT("lodBias"), 0));
        bool bSave = GetBoolFieldTextAuth(Params, TEXT("save"), true);
        
        if (AssetPath.IsEmpty())
        {
            TEXTURE_ERROR_RESPONSE(TEXT("assetPath is required"));
        }
        
        UTexture2D* Texture = Cast<UTexture2D>(StaticLoadObject(UTexture2D::StaticClass(), nullptr, *AssetPath));
        if (!Texture)
        {
            TEXTURE_ERROR_RESPONSE(FString::Printf(TEXT("Failed to load texture: %s"), *AssetPath));
        }
        
        Texture->LODBias = LODBias;
        Texture->UpdateResource();
        Texture->MarkPackageDirty();
        
        McpSaveTextureToDisk(Response, Texture, bSave);
        
Response->SetBoolField(TEXT("success"), true);
        Response->SetStringField(TEXT("message"), FString::Printf(TEXT("LOD bias set to %d"), LODBias));
        AddAssetVerification(Response, Texture);
        return Response;
    }
    
    if (SubAction == TEXT("configure_virtual_texture"))
    {
        // Validate that no unknown/invalid parameters are present
        TSet<FString> ValidParams = {
            TEXT("subAction"), TEXT("assetPath"), TEXT("virtualTextureStreaming"), TEXT("save")
        };
        for (const auto& Field : Params->Values)
        {
            if (!ValidParams.Contains(EARGCompat::JsonKeyToString(Field.Key)))
            {
                TEXTURE_ERROR_RESPONSE(FString::Printf(TEXT("Invalid parameter: %s"), *Field.Key));
            }
        }

        FString AssetPath = GetStringFieldTextAuth(Params, TEXT("assetPath"), TEXT(""));
        
        // SECURITY: Validate assetPath
        FString SanitizedAssetPath = SanitizeProjectRelativePath(AssetPath);
        if (SanitizedAssetPath.IsEmpty())
        {
            TEXTURE_ERROR_RESPONSE(TEXT("Invalid assetPath: contains traversal or invalid characters"));
        }
        AssetPath = SanitizedAssetPath;
        
        bool bVirtualTextureStreaming = GetBoolFieldTextAuth(Params, TEXT("virtualTextureStreaming"), false);
        bool bSave = GetBoolFieldTextAuth(Params, TEXT("save"), true);
        
        if (AssetPath.IsEmpty())
        {
            TEXTURE_ERROR_RESPONSE(TEXT("assetPath is required"));
        }
        
        UTexture2D* Texture = Cast<UTexture2D>(StaticLoadObject(UTexture2D::StaticClass(), nullptr, *AssetPath));
        if (!Texture)
        {
            TEXTURE_ERROR_RESPONSE(FString::Printf(TEXT("Failed to load texture: %s"), *AssetPath));
        }
        
        Texture->VirtualTextureStreaming = bVirtualTextureStreaming;
        Texture->UpdateResource();
        Texture->MarkPackageDirty();
        
        McpSaveTextureToDisk(Response, Texture, bSave);
        
        Response->SetBoolField(TEXT("success"), true);
        Response->SetStringField(TEXT("message"), FString::Printf(TEXT("Virtual texture streaming %s"), bVirtualTextureStreaming ? TEXT("enabled") : TEXT("disabled")));
        return Response;
    }
    
    if (SubAction == TEXT("set_streaming_priority"))
    {
        // Validate that no unknown/invalid parameters are present
        TSet<FString> ValidParams = {
            TEXT("subAction"), TEXT("assetPath"), TEXT("neverStream"), TEXT("save")
        };
        for (const auto& Field : Params->Values)
        {
            if (!ValidParams.Contains(EARGCompat::JsonKeyToString(Field.Key)))
            {
                TEXTURE_ERROR_RESPONSE(FString::Printf(TEXT("Invalid parameter: %s"), *Field.Key));
            }
        }

        FString AssetPath = GetStringFieldTextAuth(Params, TEXT("assetPath"), TEXT(""));
        
        // SECURITY: Validate assetPath
        FString SanitizedAssetPath = SanitizeProjectRelativePath(AssetPath);
        if (SanitizedAssetPath.IsEmpty())
        {
            TEXTURE_ERROR_RESPONSE(TEXT("Invalid assetPath: contains traversal or invalid characters"));
        }
        AssetPath = SanitizedAssetPath;
        
        bool bNeverStream = GetBoolFieldTextAuth(Params, TEXT("neverStream"), false);
        bool bSave = GetBoolFieldTextAuth(Params, TEXT("save"), true);
        
        if (AssetPath.IsEmpty())
        {
            TEXTURE_ERROR_RESPONSE(TEXT("assetPath is required"));
        }
        
        UTexture2D* Texture = Cast<UTexture2D>(StaticLoadObject(UTexture2D::StaticClass(), nullptr, *AssetPath));
        if (!Texture)
        {
            TEXTURE_ERROR_RESPONSE(FString::Printf(TEXT("Failed to load texture: %s"), *AssetPath));
        }
        
        Texture->NeverStream = bNeverStream;
        Texture->UpdateResource();
        Texture->MarkPackageDirty();
        
        McpSaveTextureToDisk(Response, Texture, bSave);
        
        Response->SetBoolField(TEXT("success"), true);
        Response->SetStringField(TEXT("message"), TEXT("Streaming priority configured"));
        return Response;
    }
    
    if (SubAction == TEXT("get_texture_info"))
    {
        // Validate that no unknown/invalid parameters are present
        TSet<FString> ValidParams = {
            TEXT("subAction"), TEXT("assetPath")
        };
        for (const auto& Field : Params->Values)
        {
            if (!ValidParams.Contains(EARGCompat::JsonKeyToString(Field.Key)))
            {
                TEXTURE_ERROR_RESPONSE(FString::Printf(TEXT("Invalid parameter: %s"), *Field.Key));
            }
        }

        FString AssetPath = GetStringFieldTextAuth(Params, TEXT("assetPath"), TEXT(""));
        
        // SECURITY: Validate assetPath
        FString SanitizedAssetPath = SanitizeProjectRelativePath(AssetPath);
        if (SanitizedAssetPath.IsEmpty())
        {
            TEXTURE_ERROR_RESPONSE(TEXT("Invalid assetPath: contains traversal or invalid characters"));
        }
        AssetPath = SanitizedAssetPath;
        
        if (AssetPath.IsEmpty())
        {
            TEXTURE_ERROR_RESPONSE(TEXT("assetPath is required"));
        }
        
        UTexture2D* Texture = Cast<UTexture2D>(StaticLoadObject(UTexture2D::StaticClass(), nullptr, *AssetPath));
        if (!Texture)
        {
            TEXTURE_ERROR_RESPONSE(FString::Printf(TEXT("Failed to load texture: %s"), *AssetPath));
        }
        
        TSharedPtr<FJsonObject> TextureInfo = MakeShared<FJsonObject>();
        TextureInfo->SetNumberField(TEXT("width"), Texture->GetSizeX());
        TextureInfo->SetNumberField(TEXT("height"), Texture->GetSizeY());
        TextureInfo->SetStringField(TEXT("format"), GPixelFormats[Texture->GetPixelFormat()].Name);
        TextureInfo->SetNumberField(TEXT("mipCount"), Texture->GetNumMips());
        TextureInfo->SetBoolField(TEXT("sRGB"), Texture->SRGB);
        TextureInfo->SetBoolField(TEXT("virtualTextureStreaming"), Texture->VirtualTextureStreaming);
        TextureInfo->SetBoolField(TEXT("neverStream"), Texture->NeverStream);
        TextureInfo->SetNumberField(TEXT("lodBias"), Texture->LODBias);
        
        // Compression settings as string
        FString CompressionStr;
        switch (Texture->CompressionSettings)
        {
            case TC_Default: CompressionStr = TEXT("TC_Default"); break;
            case TC_Normalmap: CompressionStr = TEXT("TC_Normalmap"); break;
            case TC_Masks: CompressionStr = TEXT("TC_Masks"); break;
            case TC_Grayscale: CompressionStr = TEXT("TC_Grayscale"); break;
            case TC_Displacementmap: CompressionStr = TEXT("TC_Displacementmap"); break;
            case TC_VectorDisplacementmap: CompressionStr = TEXT("TC_VectorDisplacementmap"); break;
            case TC_HDR: CompressionStr = TEXT("TC_HDR"); break;
            case TC_EditorIcon: CompressionStr = TEXT("TC_EditorIcon"); break;
            case TC_Alpha: CompressionStr = TEXT("TC_Alpha"); break;
            case TC_DistanceFieldFont: CompressionStr = TEXT("TC_DistanceFieldFont"); break;
            case TC_HDR_Compressed: CompressionStr = TEXT("TC_HDR_Compressed"); break;
            case TC_BC7: CompressionStr = TEXT("TC_BC7"); break;
            default: CompressionStr = TEXT("Unknown"); break;
        }
        TextureInfo->SetStringField(TEXT("compression"), CompressionStr);
        
        Response->SetBoolField(TEXT("success"), true);
        Response->SetStringField(TEXT("message"), TEXT("Texture info retrieved"));
        Response->SetObjectField(TEXT("textureInfo"), TextureInfo);
        return Response;
    }
    
    // ===== TEXTURE PROCESSING =====
    // Real CPU-based pixel manipulation implementations
    
    if (SubAction == TEXT("resize_texture"))
    {
        // Validate that no unknown/invalid parameters are present
        TSet<FString> ValidParams = {
            TEXT("subAction"), TEXT("sourcePath"), TEXT("name"), TEXT("path"),
            TEXT("newWidth"), TEXT("newHeight"), TEXT("save")
        };
        for (const auto& Field : Params->Values)
        {
            if (!ValidParams.Contains(EARGCompat::JsonKeyToString(Field.Key)))
            {
                TEXTURE_ERROR_RESPONSE(FString::Printf(TEXT("Invalid parameter: %s"), *Field.Key));
            }
        }

        FString SourcePath = GetStringFieldTextAuth(Params, TEXT("sourcePath"), TEXT(""));
        FString Name = GetStringFieldTextAuth(Params, TEXT("name"), TEXT(""));
        FString Path = GetStringFieldTextAuth(Params, TEXT("path"), TEXT(""));
        
        // SECURITY: Validate sourcePath
        FString SanitizedSource = SanitizeProjectRelativePath(SourcePath);
        if (SanitizedSource.IsEmpty())
        {
            TEXTURE_ERROR_RESPONSE(TEXT("Invalid sourcePath: contains traversal or invalid characters"));
        }
        SourcePath = SanitizedSource;
        
        int32 NewWidth = static_cast<int32>(GetNumberFieldTextAuth(Params, TEXT("newWidth"), 512));
        int32 NewHeight = static_cast<int32>(GetNumberFieldTextAuth(Params, TEXT("newHeight"), 512));
        bool bSave = GetBoolFieldTextAuth(Params, TEXT("save"), true);
        
        if (SourcePath.IsEmpty())
        {
            TEXTURE_ERROR_RESPONSE(TEXT("sourcePath is required"));
        }
        
        UTexture2D* SourceTexture = Cast<UTexture2D>(StaticLoadObject(UTexture2D::StaticClass(), nullptr, *SourcePath));
        if (!SourceTexture)
        {
            TEXTURE_ERROR_RESPONSE(FString::Printf(TEXT("Failed to load source texture: %s"), *SourcePath));
        }
        
        // Generate output name and path if not specified
        if (Name.IsEmpty())
        {
            Name = FPaths::GetBaseFilename(SourcePath) + TEXT("_Resized");
        }
        if (Path.IsEmpty())
        {
            Path = FPaths::GetPath(SourcePath);
        }

        // SECURITY: Validate output path
        FString SanitizedPath = SanitizeProjectRelativePath(Path);
        if (SanitizedPath.IsEmpty())
        {
            TEXTURE_ERROR_RESPONSE(TEXT("Invalid path: contains traversal or invalid characters"));
        }
        Path = SanitizedPath;

        // Validate name
        FString SanitizedName = SanitizeAssetName(Name);
        if (SanitizedName.IsEmpty())
        {
            TEXTURE_ERROR_RESPONSE(TEXT("Invalid name: contains invalid characters"));
        }
        Name = SanitizedName;

        // Create the destination BEFORE locking anything. CreateEmptyTexture runs
        // FTextureSource::Init, which asserts (Fatal) on a locked source — and NewObject
        // re-allocates in place when the output name resolves to an existing asset, which can
        // be the input itself. Creating first means that degenerate case ends as a refused
        // lock below instead of killing the editor.
        UTexture2D* NewTexture = CreateEmptyTexture(Path, Name, NewWidth, NewHeight, false);
        if (!NewTexture)
        {
            TEXTURE_ERROR_RESPONSE(TEXT("Failed to create resized texture"));
        }

        // Read the editor source (CPU-resident, uncompressed), never the built platform mip:
        // that one is the compressed / GPU-side copy, its BulkData lock is taken even when the
        // read returns null, and skipping the unlock on that path is what left the payload
        // locked and killed the editor on the next call
        // (board B-combine-textures-leaks-bulkdata-lock-then-crashes). Dimensions come off the
        // locked mip so the loop bounds and the bytes can never disagree — UTexture2D::GetSizeX
        // reports the PLATFORM size, which an LOD bias can shrink below the source.
        // Both guards release on every return below; there are no manual unlocks left.
        TextureSourceMip::FScopedMipLock SrcLock(
            SourceTexture, TEXT("source"), /*bReadOnly=*/true,
            TextureSourceMip::EFormatPolicy::Bgra8Only);
        if (!SrcLock.IsValid())
        {
            TEXTURE_ERROR_RESPONSE(SrcLock.GetError());
        }
        const FColor* SrcData = reinterpret_cast<const FColor*>(SrcLock.GetReadData());
        const int32 SrcWidth = SrcLock.GetSizeX();
        const int32 SrcHeight = SrcLock.GetSizeY();

        TextureSourceMip::FScopedMipLock DstLock(
            NewTexture, TEXT("destination"), /*bReadOnly=*/false, TextureSourceMip::EFormatPolicy::Any);
        if (!DstLock.IsValid())
        {
            TEXTURE_ERROR_RESPONSE(DstLock.GetError());
        }
        uint8* DstMipData = DstLock.GetWriteData();

        // Bilinear interpolation resize
        for (int32 Y = 0; Y < NewHeight; ++Y)
        {
            for (int32 X = 0; X < NewWidth; ++X)
            {
                float U = static_cast<float>(X) / static_cast<float>(NewWidth - 1) * (SrcWidth - 1);
                float V = static_cast<float>(Y) / static_cast<float>(NewHeight - 1) * (SrcHeight - 1);
                
                int32 X0 = FMath::FloorToInt(U);
                int32 Y0 = FMath::FloorToInt(V);
                int32 X1 = FMath::Min(X0 + 1, SrcWidth - 1);
                int32 Y1 = FMath::Min(Y0 + 1, SrcHeight - 1);
                
                float FracX = U - X0;
                float FracY = V - Y0;
                
                const FColor& C00 = SrcData[Y0 * SrcWidth + X0];
                const FColor& C10 = SrcData[Y0 * SrcWidth + X1];
                const FColor& C01 = SrcData[Y1 * SrcWidth + X0];
                const FColor& C11 = SrcData[Y1 * SrcWidth + X1];
                
                // Bilinear interpolation
                FColor SampledColor;
                SampledColor.R = static_cast<uint8>(FMath::Lerp(FMath::Lerp((float)C00.R, (float)C10.R, FracX), FMath::Lerp((float)C01.R, (float)C11.R, FracX), FracY));
                SampledColor.G = static_cast<uint8>(FMath::Lerp(FMath::Lerp((float)C00.G, (float)C10.G, FracX), FMath::Lerp((float)C01.G, (float)C11.G, FracX), FracY));
                SampledColor.B = static_cast<uint8>(FMath::Lerp(FMath::Lerp((float)C00.B, (float)C10.B, FracX), FMath::Lerp((float)C01.B, (float)C11.B, FracX), FracY));
                SampledColor.A = static_cast<uint8>(FMath::Lerp(FMath::Lerp((float)C00.A, (float)C10.A, FracX), FMath::Lerp((float)C01.A, (float)C11.A, FracX), FracY));
                
                int32 DstIndex = (Y * NewWidth + X) * 4;
                DstMipData[DstIndex + 0] = SampledColor.B;
                DstMipData[DstIndex + 1] = SampledColor.G;
                DstMipData[DstIndex + 2] = SampledColor.R;
                DstMipData[DstIndex + 3] = SampledColor.A;
            }
        }
        
        SrcLock.Release();
        DstLock.Release();
        NewTexture->UpdateResource();
        
        McpSaveTextureToDisk(Response, NewTexture, bSave);
        
        Response->SetBoolField(TEXT("success"), true);
        Response->SetStringField(TEXT("message"), FString::Printf(TEXT("Texture resized to %dx%d"), NewWidth, NewHeight));
        Response->SetStringField(TEXT("assetPath"), Path / Name);
        return Response;
    }
    
    if (SubAction == TEXT("invert"))
    {
        // Validate that no unknown/invalid parameters are present
        TSet<FString> ValidParams = {
            TEXT("subAction"), TEXT("assetPath"), TEXT("inPlace"), TEXT("name"), TEXT("path"), TEXT("save")
        };
        for (const auto& Field : Params->Values)
        {
            if (!ValidParams.Contains(EARGCompat::JsonKeyToString(Field.Key)))
            {
                TEXTURE_ERROR_RESPONSE(FString::Printf(TEXT("Invalid parameter: %s"), *Field.Key));
            }
        }

        FString AssetPath = GetStringFieldTextAuth(Params, TEXT("assetPath"), TEXT(""));
        
        // SECURITY: Validate assetPath
        FString SanitizedAssetPath = SanitizeProjectRelativePath(AssetPath);
        if (SanitizedAssetPath.IsEmpty())
        {
            TEXTURE_ERROR_RESPONSE(TEXT("Invalid assetPath: contains traversal or invalid characters"));
        }
        AssetPath = SanitizedAssetPath;
        
        bool bInPlace = GetBoolFieldTextAuth(Params, TEXT("inPlace"), true);
        FString Name = GetStringFieldTextAuth(Params, TEXT("name"), TEXT(""));
        FString Path = GetStringFieldTextAuth(Params, TEXT("path"), TEXT(""));
        bool bSave = GetBoolFieldTextAuth(Params, TEXT("save"), true);
        
        if (AssetPath.IsEmpty())
        {
            TEXTURE_ERROR_RESPONSE(TEXT("assetPath is required"));
        }
        
        UTexture2D* SourceTexture = Cast<UTexture2D>(StaticLoadObject(UTexture2D::StaticClass(), nullptr, *AssetPath));
        if (!SourceTexture)
        {
            TEXTURE_ERROR_RESPONSE(FString::Printf(TEXT("Failed to load texture: %s"), *AssetPath));
        }
        
        // Size the operation off the EDITABLE source — the buffer the loop below actually
        // reads and writes. UTexture2D::GetSizeX() reports the PLATFORM size, which an LOD
        // bias or a max-texture-size clamp can shrink below it.
        int32 Width = static_cast<int32>(SourceTexture->Source.GetSizeX());
        int32 Height = static_cast<int32>(SourceTexture->Source.GetSizeY());
        if (Width <= 0 || Height <= 0)
        {
            TEXTURE_ERROR_RESPONSE(FString::Printf(
                TEXT("Texture '%s' has no editable source data to edit; re-import or re-create it. ")
                TEXT("Retrying will not help."), *AssetPath));
        }

        UTexture2D* TargetTexture = SourceTexture;
        if (!bInPlace)
        {
            if (Name.IsEmpty()) Name = FPaths::GetBaseFilename(AssetPath) + TEXT("_Inverted");
            if (Path.IsEmpty()) Path = FPaths::GetPath(AssetPath);
            
            // SECURITY: Validate output path
            FString SanitizedPath = SanitizeProjectRelativePath(Path);
            if (SanitizedPath.IsEmpty())
            {
                TEXTURE_ERROR_RESPONSE(TEXT("Invalid path: contains traversal or invalid characters"));
            }
            Path = SanitizedPath;
            
            // Validate name
            FString SanitizedName = SanitizeAssetName(Name);
            if (SanitizedName.IsEmpty())
            {
                TEXTURE_ERROR_RESPONSE(TEXT("Invalid name: contains invalid characters"));
            }
            Name = SanitizedName;
            
            TargetTexture = CreateEmptyTexture(Path, Name, Width, Height, false);
            if (!TargetTexture)
            {
                TEXTURE_ERROR_RESPONSE(TEXT("Failed to create output texture"));
            }
        }
        
        // Lock mip data
        TextureSourceMip::FScopedMipLock MipLock(
            TargetTexture, TEXT("target"), /*bReadOnly=*/false, TextureSourceMip::EFormatPolicy::Bgra8Only);
        if (!MipLock.IsValid())
        {
            TEXTURE_ERROR_RESPONSE(MipLock.GetError());
        }
        uint8* MipData = MipLock.GetWriteData();

        if (!bInPlace)
        {
            // Copy the pixels from the input's EDITABLE source. The built platform mip this
            // used to read is the compressed / GPU-side copy: its read returns null while its
            // FBulkData lock is still taken, which both null-dereferenced this memcpy and
            // leaked the lock (board B-combine-textures-leaks-bulkdata-lock-then-crashes).
            TextureSourceMip::FScopedMipLock SrcLock(
                SourceTexture, TEXT("source"), /*bReadOnly=*/true,
                TextureSourceMip::EFormatPolicy::Bgra8Only);
            if (!SrcLock.IsValid())
            {
                TEXTURE_ERROR_RESPONSE(SrcLock.GetError());
            }
            FMemory::Memcpy(MipData, SrcLock.GetReadData(),
                static_cast<SIZE_T>(FMath::Min(SrcLock.GetMipSizeBytes(), MipLock.GetMipSizeBytes())));
        }

        // Invert RGB, keep alpha
        int32 NumPixels = Width * Height;
        for (int32 i = 0; i < NumPixels; ++i)
        {
            int32 Idx = i * 4;
            MipData[Idx + 0] = 255 - MipData[Idx + 0]; // B
            MipData[Idx + 1] = 255 - MipData[Idx + 1]; // G
            MipData[Idx + 2] = 255 - MipData[Idx + 2]; // R
            // Alpha unchanged
        }
        
        MipLock.Release();
        TargetTexture->UpdateResource();
        TargetTexture->MarkPackageDirty();

        McpSaveTextureToDisk(Response, TargetTexture, bSave);

        Response->SetBoolField(TEXT("success"), true);
        Response->SetStringField(TEXT("message"), TEXT("Texture colors inverted"));
        Response->SetStringField(TEXT("assetPath"), bInPlace ? AssetPath : (Path / Name));
        return Response;
    }
    
    if (SubAction == TEXT("desaturate"))
    {
        // Validate that no unknown/invalid parameters are present
        TSet<FString> ValidParams = {
            TEXT("subAction"), TEXT("assetPath"), TEXT("amount"), TEXT("inPlace"),
            TEXT("name"), TEXT("path"), TEXT("save")
        };
        for (const auto& Field : Params->Values)
        {
            if (!ValidParams.Contains(EARGCompat::JsonKeyToString(Field.Key)))
            {
                TEXTURE_ERROR_RESPONSE(FString::Printf(TEXT("Invalid parameter: %s"), *Field.Key));
            }
        }

        FString AssetPath = GetStringFieldTextAuth(Params, TEXT("assetPath"), TEXT(""));
        
        // SECURITY: Validate assetPath
        FString SanitizedAssetPath = SanitizeProjectRelativePath(AssetPath);
        if (SanitizedAssetPath.IsEmpty())
        {
            TEXTURE_ERROR_RESPONSE(TEXT("Invalid assetPath: contains traversal or invalid characters"));
        }
        AssetPath = SanitizedAssetPath;
        
        float Amount = static_cast<float>(GetNumberFieldTextAuth(Params, TEXT("amount"), 1.0));
        bool bInPlace = GetBoolFieldTextAuth(Params, TEXT("inPlace"), true);
        FString Name = GetStringFieldTextAuth(Params, TEXT("name"), TEXT(""));
        FString Path = GetStringFieldTextAuth(Params, TEXT("path"), TEXT(""));
        bool bSave = GetBoolFieldTextAuth(Params, TEXT("save"), true);
        
        if (AssetPath.IsEmpty())
        {
            TEXTURE_ERROR_RESPONSE(TEXT("assetPath is required"));
        }
        
        UTexture2D* SourceTexture = Cast<UTexture2D>(StaticLoadObject(UTexture2D::StaticClass(), nullptr, *AssetPath));
        if (!SourceTexture)
        {
            TEXTURE_ERROR_RESPONSE(FString::Printf(TEXT("Failed to load texture: %s"), *AssetPath));
        }
        
        // Dimensions off the editable source, not the platform size (see invert above).
        int32 Width = static_cast<int32>(SourceTexture->Source.GetSizeX());
        int32 Height = static_cast<int32>(SourceTexture->Source.GetSizeY());
        if (Width <= 0 || Height <= 0)
        {
            TEXTURE_ERROR_RESPONSE(FString::Printf(
                TEXT("Texture '%s' has no editable source data to edit; re-import or re-create it. ")
                TEXT("Retrying will not help."), *AssetPath));
        }

        UTexture2D* TargetTexture = SourceTexture;
        if (!bInPlace)
        {
            if (Name.IsEmpty()) Name = FPaths::GetBaseFilename(AssetPath) + TEXT("_Desaturated");
            if (Path.IsEmpty()) Path = FPaths::GetPath(AssetPath);
            
            // SECURITY: Validate output path
            FString SanitizedPath = SanitizeProjectRelativePath(Path);
            if (SanitizedPath.IsEmpty())
            {
                TEXTURE_ERROR_RESPONSE(TEXT("Invalid path: contains traversal or invalid characters"));
            }
            Path = SanitizedPath;
            
            // Validate name
            FString SanitizedName = SanitizeAssetName(Name);
            if (SanitizedName.IsEmpty())
            {
                TEXTURE_ERROR_RESPONSE(TEXT("Invalid name: contains invalid characters"));
            }
            Name = SanitizedName;
            
            TargetTexture = CreateEmptyTexture(Path, Name, Width, Height, false);
            if (!TargetTexture)
            {
                TEXTURE_ERROR_RESPONSE(TEXT("Failed to create output texture"));
            }
        }
        
        TextureSourceMip::FScopedMipLock MipLock(
            TargetTexture, TEXT("target"), /*bReadOnly=*/false, TextureSourceMip::EFormatPolicy::Bgra8Only);
        if (!MipLock.IsValid())
        {
            TEXTURE_ERROR_RESPONSE(MipLock.GetError());
        }
        uint8* MipData = MipLock.GetWriteData();

        if (!bInPlace)
        {
            // Editable source, never the built platform mip (see invert above).
            TextureSourceMip::FScopedMipLock SrcLock(
                SourceTexture, TEXT("source"), /*bReadOnly=*/true,
                TextureSourceMip::EFormatPolicy::Bgra8Only);
            if (!SrcLock.IsValid())
            {
                TEXTURE_ERROR_RESPONSE(SrcLock.GetError());
            }
            FMemory::Memcpy(MipData, SrcLock.GetReadData(),
                static_cast<SIZE_T>(FMath::Min(SrcLock.GetMipSizeBytes(), MipLock.GetMipSizeBytes())));
        }

        Amount = FMath::Clamp(Amount, 0.0f, 1.0f);
        int32 NumPixels = Width * Height;
        for (int32 i = 0; i < NumPixels; ++i)
        {
            int32 Idx = i * 4;
            uint8 B = MipData[Idx + 0];
            uint8 G = MipData[Idx + 1];
            uint8 R = MipData[Idx + 2];
            
            // Rec. 709 luminance coefficients
            uint8 Gray = static_cast<uint8>(Rec709Luma(R, G, B));
            
            MipData[Idx + 0] = static_cast<uint8>(FMath::Lerp((float)B, (float)Gray, Amount));
            MipData[Idx + 1] = static_cast<uint8>(FMath::Lerp((float)G, (float)Gray, Amount));
            MipData[Idx + 2] = static_cast<uint8>(FMath::Lerp((float)R, (float)Gray, Amount));
        }
        
        MipLock.Release();
        TargetTexture->UpdateResource();
        TargetTexture->MarkPackageDirty();

        McpSaveTextureToDisk(Response, TargetTexture, bSave);

        Response->SetBoolField(TEXT("success"), true);
        Response->SetStringField(TEXT("message"), FString::Printf(TEXT("Texture desaturated (amount: %.2f)"), Amount));
        Response->SetStringField(TEXT("assetPath"), bInPlace ? AssetPath : (Path / Name));
        return Response;
    }
    
    if (SubAction == TEXT("adjust_levels"))
    {
        // Validate that no unknown/invalid parameters are present
        TSet<FString> ValidParams = {
            TEXT("subAction"), TEXT("assetPath"), TEXT("inBlack"), TEXT("inWhite"),
            TEXT("gamma"), TEXT("outBlack"), TEXT("outWhite"), TEXT("inPlace"), TEXT("save")
        };
        for (const auto& Field : Params->Values)
        {
            if (!ValidParams.Contains(EARGCompat::JsonKeyToString(Field.Key)))
            {
                TEXTURE_ERROR_RESPONSE(FString::Printf(TEXT("Invalid parameter: %s"), *Field.Key));
            }
        }

        FString AssetPath = GetStringFieldTextAuth(Params, TEXT("assetPath"), TEXT(""));
        
        // SECURITY: Validate assetPath
        FString SanitizedAssetPath = SanitizeProjectRelativePath(AssetPath);
        if (SanitizedAssetPath.IsEmpty())
        {
            TEXTURE_ERROR_RESPONSE(TEXT("Invalid assetPath: contains traversal or invalid characters"));
        }
        AssetPath = SanitizedAssetPath;
        
        float InBlack = static_cast<float>(GetNumberFieldTextAuth(Params, TEXT("inBlack"), 0.0));
        float InWhite = static_cast<float>(GetNumberFieldTextAuth(Params, TEXT("inWhite"), 1.0));
        float Gamma = static_cast<float>(GetNumberFieldTextAuth(Params, TEXT("gamma"), 1.0));
        float OutBlack = static_cast<float>(GetNumberFieldTextAuth(Params, TEXT("outBlack"), 0.0));
        float OutWhite = static_cast<float>(GetNumberFieldTextAuth(Params, TEXT("outWhite"), 1.0));
        bool bInPlace = GetBoolFieldTextAuth(Params, TEXT("inPlace"), true);
        bool bSave = GetBoolFieldTextAuth(Params, TEXT("save"), true);
        
        if (AssetPath.IsEmpty())
        {
            TEXTURE_ERROR_RESPONSE(TEXT("assetPath is required"));
        }
        
        UTexture2D* Texture = Cast<UTexture2D>(StaticLoadObject(UTexture2D::StaticClass(), nullptr, *AssetPath));
        if (!Texture)
        {
            TEXTURE_ERROR_RESPONSE(FString::Printf(TEXT("Failed to load texture: %s"), *AssetPath));
        }
        
        TextureSourceMip::FScopedMipLock MipLock(
            Texture, TEXT("target"), /*bReadOnly=*/false, TextureSourceMip::EFormatPolicy::Bgra8Only);
        if (!MipLock.IsValid())
        {
            TEXTURE_ERROR_RESPONSE(MipLock.GetError());
        }
        uint8* MipData = MipLock.GetWriteData();
        // Loop bounds off the locked buffer, not the platform size GetSizeX() reports.
        int32 Width = MipLock.GetSizeX();
        int32 Height = MipLock.GetSizeY();

        InBlack = FMath::Clamp(InBlack, 0.0f, 1.0f);
        InWhite = FMath::Clamp(InWhite, 0.0f, 1.0f);
        Gamma = FMath::Max(Gamma, 0.01f);
        OutBlack = FMath::Clamp(OutBlack, 0.0f, 1.0f);
        OutWhite = FMath::Clamp(OutWhite, 0.0f, 1.0f);
        
        float InRange = FMath::Max(InWhite - InBlack, 0.001f);
        float OutRange = OutWhite - OutBlack;
        float InvGamma = 1.0f / Gamma;
        
        int32 NumPixels = Width * Height;
        for (int32 i = 0; i < NumPixels; ++i)
        {
            int32 Idx = i * 4;
            for (int32 c = 0; c < 3; ++c)
            {
                float Val = MipData[Idx + c] / 255.0f;
                Val = FMath::Clamp((Val - InBlack) / InRange, 0.0f, 1.0f);
                Val = FMath::Pow(Val, InvGamma);
                Val = OutBlack + Val * OutRange;
                MipData[Idx + c] = static_cast<uint8>(FMath::Clamp(Val * 255.0f, 0.0f, 255.0f));
            }
        }
        
        MipLock.Release();
        Texture->UpdateResource();
        Texture->MarkPackageDirty();

        McpSaveTextureToDisk(Response, Texture, bSave);

        Response->SetBoolField(TEXT("success"), true);
        Response->SetStringField(TEXT("message"), TEXT("Levels adjusted"));
        Response->SetStringField(TEXT("assetPath"), AssetPath);
        return Response;
    }
    
    if (SubAction == TEXT("blur"))
    {
        FString AssetPath = NormalizeContentAssetPath(GetStringFieldTextAuth(Params, TEXT("assetPath"), TEXT("")));
        int32 Radius = static_cast<int32>(GetNumberFieldTextAuth(Params, TEXT("radius"), 2));
        bool bSave = GetBoolFieldTextAuth(Params, TEXT("save"), true);
        
        if (AssetPath.IsEmpty())
        {
            TEXTURE_ERROR_RESPONSE(TEXT("assetPath is required"));
        }
        
        UTexture2D* Texture = Cast<UTexture2D>(StaticLoadObject(UTexture2D::StaticClass(), nullptr, *AssetPath));
        if (!Texture)
        {
            TEXTURE_ERROR_RESPONSE(FString::Printf(TEXT("Failed to load texture: %s"), *AssetPath));
        }
        
        Radius = FMath::Clamp(Radius, 1, 10);

        TextureSourceMip::FScopedMipLock MipLock(
            Texture, TEXT("target"), /*bReadOnly=*/false, TextureSourceMip::EFormatPolicy::Bgra8Only);
        if (!MipLock.IsValid())
        {
            TEXTURE_ERROR_RESPONSE(MipLock.GetError());
        }
        uint8* MipData = MipLock.GetWriteData();
        int32 Width = MipLock.GetSizeX();
        int32 Height = MipLock.GetSizeY();

        // Create copy of original data
        TArray<uint8> OriginalData;
        int32 DataSize = Width * Height * 4;
        OriginalData.SetNumUninitialized(DataSize);
        FMemory::Memcpy(OriginalData.GetData(), MipData, DataSize);
        
        // Box blur
        int32 KernelSize = Radius * 2 + 1;
        float KernelWeight = 1.0f / (KernelSize * KernelSize);
        
        for (int32 Y = 0; Y < Height; ++Y)
        {
            for (int32 X = 0; X < Width; ++X)
            {
                float SumR = 0, SumG = 0, SumB = 0;
                
                for (int32 KY = -Radius; KY <= Radius; ++KY)
                {
                    for (int32 KX = -Radius; KX <= Radius; ++KX)
                    {
                        int32 SampleX = FMath::Clamp(X + KX, 0, Width - 1);
                        int32 SampleY = FMath::Clamp(Y + KY, 0, Height - 1);
                        int32 SampleIdx = (SampleY * Width + SampleX) * 4;
                        
                        SumB += OriginalData[SampleIdx + 0];
                        SumG += OriginalData[SampleIdx + 1];
                        SumR += OriginalData[SampleIdx + 2];
                    }
                }
                
                int32 DstIdx = (Y * Width + X) * 4;
                MipData[DstIdx + 0] = static_cast<uint8>(SumB * KernelWeight);
                MipData[DstIdx + 1] = static_cast<uint8>(SumG * KernelWeight);
                MipData[DstIdx + 2] = static_cast<uint8>(SumR * KernelWeight);
            }
        }
        
        MipLock.Release();
        Texture->UpdateResource();
        Texture->MarkPackageDirty();

        McpSaveTextureToDisk(Response, Texture, bSave);

        Response->SetBoolField(TEXT("success"), true);
        Response->SetStringField(TEXT("message"), FString::Printf(TEXT("Blur applied (radius: %d)"), Radius));
        Response->SetStringField(TEXT("assetPath"), AssetPath);
        return Response;
    }
    
    if (SubAction == TEXT("sharpen"))
    {
        FString AssetPath = NormalizeContentAssetPath(GetStringFieldTextAuth(Params, TEXT("assetPath"), TEXT("")));
        float Amount = static_cast<float>(GetNumberFieldTextAuth(Params, TEXT("amount"), 1.0));
        bool bSave = GetBoolFieldTextAuth(Params, TEXT("save"), true);
        
        if (AssetPath.IsEmpty())
        {
            TEXTURE_ERROR_RESPONSE(TEXT("assetPath is required"));
        }
        
        UTexture2D* Texture = Cast<UTexture2D>(StaticLoadObject(UTexture2D::StaticClass(), nullptr, *AssetPath));
        if (!Texture)
        {
            TEXTURE_ERROR_RESPONSE(FString::Printf(TEXT("Failed to load texture: %s"), *AssetPath));
        }
        
        Amount = FMath::Clamp(Amount, 0.0f, 5.0f);

        TextureSourceMip::FScopedMipLock MipLock(
            Texture, TEXT("target"), /*bReadOnly=*/false, TextureSourceMip::EFormatPolicy::Bgra8Only);
        if (!MipLock.IsValid())
        {
            TEXTURE_ERROR_RESPONSE(MipLock.GetError());
        }
        uint8* MipData = MipLock.GetWriteData();
        int32 Width = MipLock.GetSizeX();
        int32 Height = MipLock.GetSizeY();

        // Create copy of original data
        TArray<uint8> OriginalData;
        int32 DataSize = Width * Height * 4;
        OriginalData.SetNumUninitialized(DataSize);
        FMemory::Memcpy(OriginalData.GetData(), MipData, DataSize);
        
        // Unsharp mask sharpening
        // Sharpen kernel: center = 1 + 4*amount, neighbors = -amount
        for (int32 Y = 1; Y < Height - 1; ++Y)
        {
            for (int32 X = 1; X < Width - 1; ++X)
            {
                int32 CenterIdx = (Y * Width + X) * 4;
                int32 LeftIdx = (Y * Width + X - 1) * 4;
                int32 RightIdx = (Y * Width + X + 1) * 4;
                int32 TopIdx = ((Y - 1) * Width + X) * 4;
                int32 BottomIdx = ((Y + 1) * Width + X) * 4;
                
                for (int32 c = 0; c < 3; ++c)
                {
                    float Center = OriginalData[CenterIdx + c];
                    float Left = OriginalData[LeftIdx + c];
                    float Right = OriginalData[RightIdx + c];
                    float Top = OriginalData[TopIdx + c];
                    float Bottom = OriginalData[BottomIdx + c];
                    
                    float Sharpened = Center * (1.0f + 4.0f * Amount) - Amount * (Left + Right + Top + Bottom);
                    MipData[CenterIdx + c] = static_cast<uint8>(FMath::Clamp(Sharpened, 0.0f, 255.0f));
                }
            }
        }
        
        MipLock.Release();
        Texture->UpdateResource();
        Texture->MarkPackageDirty();

        McpSaveTextureToDisk(Response, Texture, bSave);

        Response->SetBoolField(TEXT("success"), true);
        Response->SetStringField(TEXT("message"), FString::Printf(TEXT("Sharpen applied (amount: %.2f)"), Amount));
        Response->SetStringField(TEXT("assetPath"), AssetPath);
        return Response;
    }
    
    if (SubAction == TEXT("channel_pack"))
    {
        FString RedPath = NormalizeContentAssetPath(GetStringFieldTextAuth(Params, TEXT("redTexture"), TEXT("")));
        FString GreenPath = NormalizeContentAssetPath(GetStringFieldTextAuth(Params, TEXT("greenTexture"), TEXT("")));
        FString BluePath = NormalizeContentAssetPath(GetStringFieldTextAuth(Params, TEXT("blueTexture"), TEXT("")));
        FString AlphaPath = NormalizeContentAssetPath(GetStringFieldTextAuth(Params, TEXT("alphaTexture"), TEXT("")));
        FString Name = GetStringFieldTextAuth(Params, TEXT("name"), TEXT("ChannelPacked"));
        FString Path = NormalizeContentAssetPath(GetStringFieldTextAuth(Params, TEXT("path"), TEXT("/Game/Textures")));
        bool bSave = GetBoolFieldTextAuth(Params, TEXT("save"), true);
        
        if (Name.IsEmpty())
        {
            TEXTURE_ERROR_RESPONSE(TEXT("name is required"));
        }

        if (RedPath.IsEmpty() && GreenPath.IsEmpty() && BluePath.IsEmpty() && AlphaPath.IsEmpty())
        {
            TEXTURE_ERROR_RESPONSE(TEXT("At least one of redTexture, greenTexture, blueTexture, or alphaTexture is required"));
        }
        
        // Load channel textures
        UTexture2D* RedTex = !RedPath.IsEmpty() ? Cast<UTexture2D>(StaticLoadObject(UTexture2D::StaticClass(), nullptr, *RedPath)) : nullptr;
        UTexture2D* GreenTex = !GreenPath.IsEmpty() ? Cast<UTexture2D>(StaticLoadObject(UTexture2D::StaticClass(), nullptr, *GreenPath)) : nullptr;
        UTexture2D* BlueTex = !BluePath.IsEmpty() ? Cast<UTexture2D>(StaticLoadObject(UTexture2D::StaticClass(), nullptr, *BluePath)) : nullptr;
        UTexture2D* AlphaTex = !AlphaPath.IsEmpty() ? Cast<UTexture2D>(StaticLoadObject(UTexture2D::StaticClass(), nullptr, *AlphaPath)) : nullptr;
        
        // Determine output size from first available texture, off its editable source (the
        // buffer the channel reads below come from), not the platform size.
        int32 Width = 1024, Height = 1024;
        if (RedTex) { Width = static_cast<int32>(RedTex->Source.GetSizeX()); Height = static_cast<int32>(RedTex->Source.GetSizeY()); }
        else if (GreenTex) { Width = static_cast<int32>(GreenTex->Source.GetSizeX()); Height = static_cast<int32>(GreenTex->Source.GetSizeY()); }
        else if (BlueTex) { Width = static_cast<int32>(BlueTex->Source.GetSizeX()); Height = static_cast<int32>(BlueTex->Source.GetSizeY()); }
        else if (AlphaTex) { Width = static_cast<int32>(AlphaTex->Source.GetSizeX()); Height = static_cast<int32>(AlphaTex->Source.GetSizeY()); }
        if (Width <= 0 || Height <= 0)
        {
            TEXTURE_ERROR_RESPONSE(TEXT("The first supplied channel texture has no editable source data to read; ")
                TEXT("re-import or re-create it. Retrying will not help."));
        }

        UTexture2D* OutputTexture = CreateEmptyTexture(Path, Name, Width, Height, false);
        if (!OutputTexture)
        {
            TEXTURE_ERROR_RESPONSE(TEXT("Failed to create output texture"));
        }

        TextureSourceMip::FScopedMipLock OutLock(
            OutputTexture, TEXT("output"), /*bReadOnly=*/false, TextureSourceMip::EFormatPolicy::Any);
        if (!OutLock.IsValid())
        {
            TEXTURE_ERROR_RESPONSE(OutLock.GetError());
        }
        uint8* OutData = OutLock.GetWriteData();

        // After the lock, whose ctor flushed the async build CreateEmptyTexture kicked off:
        // flipping the gamma-affecting flags while that build is still reading the source is
        // what crashed a compile worker in B-texture-normal-from-height-crash.
        OutputTexture->SRGB = false;
        OutputTexture->CompressionSettings = TC_Masks;

        // Read each contributing channel from its EDITABLE source. The built platform mip this
        // used to lock is the compressed / GPU-side copy: FBulkData::LockReadOnly() returns
        // null for it while still TAKING the lock, so the indexed read below dereferenced
        // address 0 and the lock leaked into the next call
        // (board B-combine-textures-leaks-bulkdata-lock-then-crashes).
        FString ChannelError;
        auto GetChannelData = [&ChannelError](UTexture2D* Tex, const TCHAR* Role, int32 ChannelIdx) -> TArray<uint8>
        {
            TArray<uint8> Data;
            if (!Tex)
            {
                return Data;
            }
            TextureSourceMip::FScopedMipLock ChannelLock(
                Tex, Role, /*bReadOnly=*/true, TextureSourceMip::EFormatPolicy::Bgra8Only);
            if (!ChannelLock.IsValid())
            {
                if (ChannelError.IsEmpty())
                {
                    ChannelError = ChannelLock.GetError();
                }
                return Data;
            }
            const uint8* MipData = ChannelLock.GetReadData();
            const int32 NumTexels = static_cast<int32>(ChannelLock.GetNumPixels());
            Data.SetNumUninitialized(NumTexels);
            for (int32 i = 0; i < NumTexels; ++i)
            {
                Data[i] = MipData[i * 4 + ChannelIdx];
            }
            return Data;
        };

        TArray<uint8> RedData = GetChannelData(RedTex, TEXT("redTexture"), 2); // R is at index 2 in BGRA
        TArray<uint8> GreenData = GetChannelData(GreenTex, TEXT("greenTexture"), 1);
        TArray<uint8> BlueData = GetChannelData(BlueTex, TEXT("blueTexture"), 0);
        TArray<uint8> AlphaData = GetChannelData(AlphaTex, TEXT("alphaTexture"), 3);
        if (!ChannelError.IsEmpty())
        {
            TEXTURE_ERROR_RESPONSE(ChannelError);
        }

        int32 NumPixels = Width * Height;
        for (int32 i = 0; i < NumPixels; ++i)
        {
            int32 Idx = i * 4;
            OutData[Idx + 0] = BlueData.Num() > i ? BlueData[i] : 0; // B
            OutData[Idx + 1] = GreenData.Num() > i ? GreenData[i] : 0; // G
            OutData[Idx + 2] = RedData.Num() > i ? RedData[i] : 0; // R
            OutData[Idx + 3] = AlphaData.Num() > i ? AlphaData[i] : 255; // A
        }
        
        OutLock.Release();
        OutputTexture->UpdateResource();

        McpSaveTextureToDisk(Response, OutputTexture, bSave);

        Response->SetBoolField(TEXT("success"), true);
        Response->SetStringField(TEXT("message"), TEXT("Channels packed into single texture"));
        Response->SetStringField(TEXT("assetPath"), Path / Name);
        return Response;
    }
    
    if (SubAction == TEXT("combine_textures"))
    {
        FString BaseTexturePath = NormalizeContentAssetPath(GetStringFieldTextAuth(Params, TEXT("baseTexture"), TEXT("")));
        FString OverlayTexturePath = NormalizeContentAssetPath(GetStringFieldTextAuth(Params, TEXT("overlayTexture"), TEXT("")));
        FString BlendMode = GetStringFieldTextAuth(Params, TEXT("blendMode"), TEXT("Normal"));
        float Opacity = static_cast<float>(GetNumberFieldTextAuth(Params, TEXT("opacity"), 1.0));
        FString Name = GetStringFieldTextAuth(Params, TEXT("name"), TEXT("Combined"));
        FString Path = NormalizeContentAssetPath(GetStringFieldTextAuth(Params, TEXT("path"), TEXT("/Game/Textures")));
        bool bSave = GetBoolFieldTextAuth(Params, TEXT("save"), true);
        
        if (BaseTexturePath.IsEmpty() || OverlayTexturePath.IsEmpty())
        {
            TEXTURE_ERROR_RESPONSE(TEXT("baseTexture and overlayTexture are required"));
        }
        
        UTexture2D* BaseTex = Cast<UTexture2D>(StaticLoadObject(UTexture2D::StaticClass(), nullptr, *BaseTexturePath));
        UTexture2D* OverlayTex = Cast<UTexture2D>(StaticLoadObject(UTexture2D::StaticClass(), nullptr, *OverlayTexturePath));
        
        if (!BaseTex || !OverlayTex)
        {
            TEXTURE_ERROR_RESPONSE(TEXT("Failed to load base or overlay texture"));
        }
        
        Opacity = FMath::Clamp(Opacity, 0.0f, 1.0f);

        // The output carries the base's dimensions, read off the base's EDITABLE source — the
        // buffer the blend below actually reads. UTexture2D::GetSizeX() reports the PLATFORM
        // size, which an LOD bias or a max-size clamp can shrink below it.
        const int32 Width = static_cast<int32>(BaseTex->Source.GetSizeX());
        const int32 Height = static_cast<int32>(BaseTex->Source.GetSizeY());
        if (Width <= 0 || Height <= 0)
        {
            TEXTURE_ERROR_RESPONSE(FString::Printf(
                TEXT("base texture '%s' has no editable source data to read; re-import or ")
                TEXT("re-create it. Retrying will not help."), *BaseTexturePath));
        }

        // Create the output BEFORE locking anything: CreateEmptyTexture runs
        // FTextureSource::Init, which asserts (Fatal) on a locked source, and NewObject
        // re-allocates in place when the output name resolves to an existing asset — possibly
        // one of the two inputs.
        UTexture2D* OutputTexture = CreateEmptyTexture(Path, Name, Width, Height, false);
        if (!OutputTexture)
        {
            TEXTURE_ERROR_RESPONSE(TEXT("Failed to create output texture"));
        }

        // This block is the one both Critical board tickets were filed against
        // (B-combine-textures-leaks-bulkdata-lock-then-crashes,
        // B-texture-action-bulkdata-lock-assert-fatal). It used to lock the built PLATFORM
        // mips: FBulkData::LockReadOnly() takes the lock and only then returns
        // GetDataBufferReadOnly(), which is null for a compressed / GPU-resident payload — so
        // the `if (BaseData) ... Unlock()` guard skipped the unlock in exactly the case where
        // the lock HAD been taken. The payload stayed locked, the soft "Failed to lock texture
        // data" invited a retry, and the retry hit check(IsUnlocked()) and killed the editor.
        // Nothing here can leak now: FScopedMipLock releases on every exit path, and a refused
        // lock never took one.
        TextureSourceMip::FScopedMipLock BaseLock(
            BaseTex, TEXT("base"), /*bReadOnly=*/true, TextureSourceMip::EFormatPolicy::Bgra8Only);
        if (!BaseLock.IsValid())
        {
            TEXTURE_ERROR_RESPONSE(BaseLock.GetError());
        }
        TextureSourceMip::FScopedMipLock OverlayLock(
            OverlayTex, TEXT("overlay"), /*bReadOnly=*/true, TextureSourceMip::EFormatPolicy::Bgra8Only);
        if (!OverlayLock.IsValid())
        {
            TEXTURE_ERROR_RESPONSE(OverlayLock.GetError());
        }
        TextureSourceMip::FScopedMipLock OutLock(
            OutputTexture, TEXT("output"), /*bReadOnly=*/false, TextureSourceMip::EFormatPolicy::Any);
        if (!OutLock.IsValid())
        {
            TEXTURE_ERROR_RESPONSE(OutLock.GetError());
        }

        const uint8* BaseData = BaseLock.GetReadData();
        const uint8* OverlayData = OverlayLock.GetReadData();
        uint8* OutData = OutLock.GetWriteData();

        // Clamp to the smallest of the three buffers: an overlay smaller than the base used to
        // read past its end.
        const int32 NumPixels = FMath::Min3(
            static_cast<int32>(BaseLock.GetNumPixels()),
            static_cast<int32>(OverlayLock.GetNumPixels()),
            static_cast<int32>(OutLock.GetNumPixels()));
        for (int32 i = 0; i < NumPixels; ++i)
        {
            int32 Idx = i * 4;
            
            for (int32 c = 0; c < 3; ++c)
            {
                float Base = BaseData[Idx + c] / 255.0f;
                float Overlay = OverlayData[Idx + c] / 255.0f;
                float Result;
                
                if (BlendMode.Equals(TEXT("Multiply"), ESearchCase::IgnoreCase))
                {
                    Result = Base * Overlay;
                }
                else if (BlendMode.Equals(TEXT("Screen"), ESearchCase::IgnoreCase))
                {
                    Result = 1.0f - (1.0f - Base) * (1.0f - Overlay);
                }
                else if (BlendMode.Equals(TEXT("Overlay"), ESearchCase::IgnoreCase))
                {
                    Result = Base < 0.5f ? 2.0f * Base * Overlay : 1.0f - 2.0f * (1.0f - Base) * (1.0f - Overlay);
                }
                else if (BlendMode.Equals(TEXT("Add"), ESearchCase::IgnoreCase))
                {
                    Result = FMath::Min(Base + Overlay, 1.0f);
                }
                else // Normal blend
                {
                    Result = Overlay;
                }
                
                Result = FMath::Lerp(Base, Result, Opacity);
                OutData[Idx + c] = static_cast<uint8>(FMath::Clamp(Result * 255.0f, 0.0f, 255.0f));
            }
            OutData[Idx + 3] = BaseData[Idx + 3]; // Keep base alpha
        }
        
        BaseLock.Release();
        OverlayLock.Release();
        OutLock.Release();
        OutputTexture->UpdateResource();
        
        McpSaveTextureToDisk(Response, OutputTexture, bSave);
        
        Response->SetBoolField(TEXT("success"), true);
        Response->SetStringField(TEXT("message"), FString::Printf(TEXT("Textures combined (mode: %s)"), *BlendMode));
        Response->SetStringField(TEXT("assetPath"), Path / Name);
        return Response;
    }
    
    // ===== adjust_curves =====
    // Apply RGB curve adjustment using LUT (lookup table) built from control points
    if (SubAction == TEXT("adjust_curves"))
    {
        FString AssetPath = NormalizeContentAssetPath(GetStringFieldTextAuth(Params, TEXT("assetPath"), TEXT("")));
        bool bInPlace = GetBoolFieldTextAuth(Params, TEXT("inPlace"), true);
        FString Name = GetStringFieldTextAuth(Params, TEXT("name"), TEXT(""));
        FString Path = NormalizeContentAssetPath(GetStringFieldTextAuth(Params, TEXT("path"), TEXT("")));
        bool bSave = GetBoolFieldTextAuth(Params, TEXT("save"), true);
        
        if (AssetPath.IsEmpty())
        {
            TEXTURE_ERROR_RESPONSE(TEXT("assetPath is required"));
        }
        
        UTexture2D* SourceTexture = Cast<UTexture2D>(StaticLoadObject(UTexture2D::StaticClass(), nullptr, *AssetPath));
        if (!SourceTexture)
        {
            TEXTURE_ERROR_RESPONSE(FString::Printf(TEXT("Failed to load texture: %s"), *AssetPath));
        }
        
        // Dimensions off the editable source, not the platform size (see invert above).
        int32 Width = static_cast<int32>(SourceTexture->Source.GetSizeX());
        int32 Height = static_cast<int32>(SourceTexture->Source.GetSizeY());
        if (Width <= 0 || Height <= 0)
        {
            TEXTURE_ERROR_RESPONSE(FString::Printf(
                TEXT("Texture '%s' has no editable source data to edit; re-import or re-create it. ")
                TEXT("Retrying will not help."), *AssetPath));
        }

        // Parse curve control points
        // Input/output arrays where input[i] maps to output[i]
        // Default: linear curve (0->0, 0.25->0.25, 0.5->0.5, 0.75->0.75, 1->1)
        TArray<float> InputPointsR, OutputPointsR;
        TArray<float> InputPointsG, OutputPointsG;
        TArray<float> InputPointsB, OutputPointsB;
        
        // Helper to parse curve points from JSON array
        auto ParseCurvePoints = [&Params](const FString& InputKey, const FString& OutputKey, TArray<float>& InputArr, TArray<float>& OutputArr) {
            const TArray<TSharedPtr<FJsonValue>>* InputArray;
            const TArray<TSharedPtr<FJsonValue>>* OutputArray;
            if (Params->TryGetArrayField(InputKey, InputArray) && Params->TryGetArrayField(OutputKey, OutputArray))
            {
                for (const auto& Val : *InputArray)
                {
                    InputArr.Add(static_cast<float>(Val->AsNumber()));
                }
                for (const auto& Val : *OutputArray)
                {
                    OutputArr.Add(static_cast<float>(Val->AsNumber()));
                }
            }
            // If not provided or empty, set default linear
            if (InputArr.Num() == 0 || OutputArr.Num() == 0)
            {
                InputArr = { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f };
                OutputArr = { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f };
            }
        };
        
        // Check if separate RGB curves are provided, otherwise use master curve
        if (Params->HasField(TEXT("inputR")))
        {
            ParseCurvePoints(TEXT("inputR"), TEXT("outputR"), InputPointsR, OutputPointsR);
            ParseCurvePoints(TEXT("inputG"), TEXT("outputG"), InputPointsG, OutputPointsG);
            ParseCurvePoints(TEXT("inputB"), TEXT("outputB"), InputPointsB, OutputPointsB);
        }
        else
        {
            // Use master curve for all channels
            TArray<float> MasterInput, MasterOutput;
            ParseCurvePoints(TEXT("input"), TEXT("output"), MasterInput, MasterOutput);
            InputPointsR = MasterInput; OutputPointsR = MasterOutput;
            InputPointsG = MasterInput; OutputPointsG = MasterOutput;
            InputPointsB = MasterInput; OutputPointsB = MasterOutput;
        }
        
        // Build 256-entry LUT via linear interpolation
        auto BuildLUT = [](const TArray<float>& Input, const TArray<float>& Output) -> TArray<uint8> {
            TArray<uint8> LUT;
            LUT.SetNum(256);
            
            if (Input.Num() < 2 || Output.Num() < 2 || Input.Num() != Output.Num())
            {
                // Fallback: linear 1:1 mapping
                for (int32 i = 0; i < 256; ++i)
                {
                    LUT[i] = static_cast<uint8>(i);
                }
                return LUT;
            }
            
            for (int32 i = 0; i < 256; ++i)
            {
                float NormalizedInput = static_cast<float>(i) / 255.0f;
                float Mapped = NormalizedInput;
                
                // Find segment in curve and interpolate
                for (int32 j = 0; j < Input.Num() - 1; ++j)
                {
                    if (NormalizedInput >= Input[j] && NormalizedInput <= Input[j + 1])
                    {
                        float SegmentRange = Input[j + 1] - Input[j];
                        if (SegmentRange > SMALL_NUMBER)
                        {
                            float T = (NormalizedInput - Input[j]) / SegmentRange;
                            Mapped = FMath::Lerp(Output[j], Output[j + 1], T);
                        }
                        else
                        {
                            Mapped = Output[j];
                        }
                        break;
                    }
                }
                
                // Handle values outside the defined range
                if (NormalizedInput < Input[0])
                {
                    Mapped = Output[0];
                }
                else if (NormalizedInput > Input[Input.Num() - 1])
                {
                    Mapped = Output[Output.Num() - 1];
                }
                
                LUT[i] = static_cast<uint8>(FMath::Clamp(Mapped * 255.0f, 0.0f, 255.0f));
            }
            return LUT;
        };
        
        TArray<uint8> LUT_R = BuildLUT(InputPointsR, OutputPointsR);
        TArray<uint8> LUT_G = BuildLUT(InputPointsG, OutputPointsG);
        TArray<uint8> LUT_B = BuildLUT(InputPointsB, OutputPointsB);
        
        UTexture2D* TargetTexture = SourceTexture;
        if (!bInPlace)
        {
            if (Name.IsEmpty()) Name = FPaths::GetBaseFilename(AssetPath) + TEXT("_Curved");
            if (Path.IsEmpty()) Path = FPaths::GetPath(AssetPath);
            TargetTexture = CreateEmptyTexture(Path, Name, Width, Height, false);
            if (!TargetTexture)
            {
                TEXTURE_ERROR_RESPONSE(TEXT("Failed to create output texture"));
            }
        }
        
        TextureSourceMip::FScopedMipLock MipLock(
            TargetTexture, TEXT("target"), /*bReadOnly=*/false, TextureSourceMip::EFormatPolicy::Bgra8Only);
        if (!MipLock.IsValid())
        {
            TEXTURE_ERROR_RESPONSE(MipLock.GetError());
        }
        uint8* MipData = MipLock.GetWriteData();

        if (!bInPlace)
        {
            // Editable source, never the built platform mip (see invert above).
            TextureSourceMip::FScopedMipLock SrcLock(
                SourceTexture, TEXT("source"), /*bReadOnly=*/true,
                TextureSourceMip::EFormatPolicy::Bgra8Only);
            if (!SrcLock.IsValid())
            {
                TEXTURE_ERROR_RESPONSE(SrcLock.GetError());
            }
            FMemory::Memcpy(MipData, SrcLock.GetReadData(),
                static_cast<SIZE_T>(FMath::Min(SrcLock.GetMipSizeBytes(), MipLock.GetMipSizeBytes())));
        }

        // Apply LUT to each pixel (BGRA format: B=0, G=1, R=2, A=3)
        int32 NumPixels = Width * Height;
        for (int32 i = 0; i < NumPixels; ++i)
        {
            int32 Idx = i * 4;
            MipData[Idx + 0] = LUT_B[MipData[Idx + 0]]; // B
            MipData[Idx + 1] = LUT_G[MipData[Idx + 1]]; // G
            MipData[Idx + 2] = LUT_R[MipData[Idx + 2]]; // R
            // Alpha unchanged
        }
        
        MipLock.Release();
        TargetTexture->UpdateResource();
        TargetTexture->MarkPackageDirty();

        McpSaveTextureToDisk(Response, TargetTexture, bSave);

        Response->SetBoolField(TEXT("success"), true);
        Response->SetStringField(TEXT("message"), TEXT("Curve adjustment applied"));
        Response->SetStringField(TEXT("assetPath"), bInPlace ? AssetPath : (Path / Name));
        return Response;
    }
    
    // ===== channel_extract =====
    // Extract a single channel (R, G, B, or A) to a new grayscale texture
    if (SubAction == TEXT("channel_extract"))
    {
        FString SourcePath = NormalizeContentAssetPath(GetStringFieldTextAuth(Params, TEXT("texturePath"), TEXT("")));
        FString Channel = GetStringFieldTextAuth(Params, TEXT("channel"), TEXT("R"));
        FString OutputPath = NormalizeContentAssetPath(GetStringFieldTextAuth(Params, TEXT("outputPath"), TEXT("")));
        FString Name = GetStringFieldTextAuth(Params, TEXT("name"), TEXT(""));
        bool bSave = GetBoolFieldTextAuth(Params, TEXT("save"), true);
        
        if (SourcePath.IsEmpty())
        {
            TEXTURE_ERROR_RESPONSE(TEXT("texturePath is required"));
        }
        
        UTexture2D* SourceTexture = Cast<UTexture2D>(StaticLoadObject(UTexture2D::StaticClass(), nullptr, *SourcePath));
        if (!SourceTexture)
        {
            TEXTURE_ERROR_RESPONSE(FString::Printf(TEXT("Failed to load source texture: %s"), *SourcePath));
        }
        
        // Dimensions off the EDITABLE source, which is also where the pixels are read from
        // below. UTexture2D::GetSizeX() reports the PLATFORM size, which an LOD bias or a
        // max-size clamp can shrink below it.
        const int32 Width = static_cast<int32>(SourceTexture->Source.GetSizeX());
        const int32 Height = static_cast<int32>(SourceTexture->Source.GetSizeY());
        if (Width <= 0 || Height <= 0)
        {
            TEXTURE_ERROR_RESPONSE(FString::Printf(
                TEXT("Source texture '%s' has no editable source data to read; re-import or ")
                TEXT("re-create it. Retrying will not help."), *SourcePath));
        }

        // Determine output path and name
        if (OutputPath.IsEmpty())
        {
            OutputPath = FPaths::GetPath(SourcePath);
        }
        if (Name.IsEmpty())
        {
            Name = FPaths::GetBaseFilename(SourcePath) + TEXT("_") + Channel;
        }
        
        // Create package for new texture. `name` and `outputPath` are both caller text and
        // neither has been checked: CreatePackage (UObjectGlobals.cpp:1087-1120) logs at Fatal -
        // never compiled out - for a name containing "//" or one that resolves to empty, which
        // ends the editor PROCESS instead of failing this call, so the `if (!Package)` branch
        // below is unreachable on that input (B-createpackage-unvalidated-paths-plugin-wide).
        // No trailing-slash trim is needed on OutputPath: it was run through
        // NormalizeContentAssetPath at read time, which strips them.
        FString FullAssetPath;
        FString PathError;
        if (!PinWrightComposeAssetPackagePath(OutputPath, Name, FullAssetPath, PathError))
        {
            Response->SetStringField(TEXT("errorCode"), TEXT("INVALID_ARGUMENT"));
            TEXTURE_ERROR_RESPONSE(FString::Printf(
                TEXT("%s Pass a bare asset name in 'name' and choose the folder with 'outputPath'."),
                *PathError));
        }

        UPackage* Package = CreatePackage(*FullAssetPath);
        if (!Package)
        {
            TEXTURE_ERROR_RESPONSE(TEXT("Failed to create package for output texture"));
        }

        // Create new texture with grayscale format (TSF_G8)
        UTexture2D* NewTexture = NewObject<UTexture2D>(Package, FName(*Name), RF_Public | RF_Standalone);
        if (!NewTexture)
        {
            TEXTURE_ERROR_RESPONSE(TEXT("Failed to create output texture"));
        }

        // Initialize source with single-channel grayscale. This runs BEFORE either lock:
        // FTextureSource::Init asserts (Fatal) on a locked source, and NewObject above
        // re-allocates in place when the output name resolves to an existing asset — possibly
        // the input itself.
        NewTexture->Source.Init(Width, Height, 1, 1, TSF_G8);

        // Read the pixels from the EDITABLE source, never the built platform mip: the platform
        // copy is compressed / GPU-side, its FBulkData lock is taken even when the read comes
        // back null, and the `if (!SrcPixels) return` that used to sit here skipped the unlock
        // on exactly that path (board B-combine-textures-leaks-bulkdata-lock-then-crashes).
        // Both guards release on every return below; the manual unlocks are gone with them.
        TextureSourceMip::FScopedMipLock SrcLock(
            SourceTexture, TEXT("source"), /*bReadOnly=*/true,
            TextureSourceMip::EFormatPolicy::Bgra8Only);
        if (!SrcLock.IsValid())
        {
            TEXTURE_ERROR_RESPONSE(SrcLock.GetError());
        }
        const FColor* SrcPixels = reinterpret_cast<const FColor*>(SrcLock.GetReadData());

        TextureSourceMip::FScopedMipLock DestLock(
            NewTexture, TEXT("destination"), /*bReadOnly=*/false, TextureSourceMip::EFormatPolicy::Any);
        if (!DestLock.IsValid())
        {
            TEXTURE_ERROR_RESPONSE(DestLock.GetError());
        }
        uint8* DestData = DestLock.GetWriteData();

        // Determine which channel to extract
        // FColor: R, G, B, A are separate uint8 members
        for (int32 i = 0; i < Width * Height; ++i)
        {
            uint8 Value;
            if (Channel.Equals(TEXT("R"), ESearchCase::IgnoreCase))
            {
                Value = SrcPixels[i].R;
            }
            else if (Channel.Equals(TEXT("G"), ESearchCase::IgnoreCase))
            {
                Value = SrcPixels[i].G;
            }
            else if (Channel.Equals(TEXT("B"), ESearchCase::IgnoreCase))
            {
                Value = SrcPixels[i].B;
            }
            else if (Channel.Equals(TEXT("A"), ESearchCase::IgnoreCase))
            {
                Value = SrcPixels[i].A;
            }
            else
            {
                // Default to R if invalid channel specified
                Value = SrcPixels[i].R;
            }
            DestData[i] = Value;
        }
        
        DestLock.Release();
        SrcLock.Release();
        
        // Set texture properties for grayscale mask
        NewTexture->SRGB = false;
        NewTexture->CompressionSettings = TC_Grayscale;
        NewTexture->MipGenSettings = TMGS_FromTextureGroup;
        NewTexture->LODGroup = TEXTUREGROUP_World;
        
        NewTexture->UpdateResource();
        Package->MarkPackageDirty();
        
        McpSaveTextureToDisk(Response, NewTexture, bSave);
        
        Response->SetBoolField(TEXT("success"), true);
        Response->SetStringField(TEXT("message"), FString::Printf(TEXT("Channel '%s' extracted to grayscale texture"), *Channel));
        Response->SetStringField(TEXT("assetPath"), FullAssetPath);
        Response->SetStringField(TEXT("channel"), Channel);
        Response->SetNumberField(TEXT("width"), Width);
        Response->SetNumberField(TEXT("height"), Height);
        return Response;
    }
    
    // ===== Additional Actions for Test Compatibility =====

    if (SubAction == TEXT("set_texture_filter"))
    {
        FString AssetPath = NormalizeContentAssetPath(GetStringFieldTextAuth(Params, TEXT("assetPath"), TEXT("")));
        FString FilterMode = GetStringFieldTextAuth(Params, TEXT("filter"), TEXT("Default"));
        bool bSave = GetBoolFieldTextAuth(Params, TEXT("save"), true);
        
        if (AssetPath.IsEmpty())
        {
            TEXTURE_ERROR_RESPONSE(TEXT("assetPath is required"));
        }
        
        UTexture2D* Texture = Cast<UTexture2D>(StaticLoadObject(UTexture2D::StaticClass(), nullptr, *AssetPath));
        if (!Texture)
        {
            TEXTURE_ERROR_RESPONSE(FString::Printf(TEXT("Failed to load texture: %s"), *AssetPath));
        }
        
        // Map filter modes
        TextureFilter Filter = TF_Default;
        if (FilterMode == TEXT("Nearest")) Filter = TF_Nearest;
        else if (FilterMode == TEXT("Bilinear")) Filter = TF_Bilinear;
        else if (FilterMode == TEXT("Trilinear")) Filter = TF_Trilinear;
        else if (FilterMode == TEXT("Default")) Filter = TF_Default;
        
        Texture->Filter = Filter;
        Texture->UpdateResource();
        Texture->MarkPackageDirty();
        
        McpSaveTextureToDisk(Response, Texture, bSave);
        
        Response->SetBoolField(TEXT("success"), true);
        Response->SetStringField(TEXT("message"), FString::Printf(TEXT("Filter set to %s"), *FilterMode));
        return Response;
    }
    
    if (SubAction == TEXT("set_texture_wrap"))
    {
        FString AssetPath = NormalizeContentAssetPath(GetStringFieldTextAuth(Params, TEXT("assetPath"), TEXT("")));
        FString WrapMode = GetStringFieldTextAuth(Params, TEXT("wrapMode"), TEXT("Wrap"));
        bool bSave = GetBoolFieldTextAuth(Params, TEXT("save"), true);
        
        if (AssetPath.IsEmpty())
        {
            TEXTURE_ERROR_RESPONSE(TEXT("assetPath is required"));
        }
        
        UTexture2D* Texture = Cast<UTexture2D>(StaticLoadObject(UTexture2D::StaticClass(), nullptr, *AssetPath));
        if (!Texture)
        {
            TEXTURE_ERROR_RESPONSE(FString::Printf(TEXT("Failed to load texture: %s"), *AssetPath));
        }
        
        // Map wrap modes
        TextureAddress WrapU = TA_Wrap, WrapV = TA_Wrap;
        if (WrapMode == TEXT("Clamp")) { WrapU = TA_Clamp; WrapV = TA_Clamp; }
        else if (WrapMode == TEXT("Mirror")) { WrapU = TA_Mirror; WrapV = TA_Mirror; }
        else if (WrapMode == TEXT("Wrap")) { WrapU = TA_Wrap; WrapV = TA_Wrap; }
        
        Texture->AddressX = WrapU;
        Texture->AddressY = WrapV;
        Texture->UpdateResource();
        Texture->MarkPackageDirty();
        
        McpSaveTextureToDisk(Response, Texture, bSave);
        
        Response->SetBoolField(TEXT("success"), true);
        Response->SetStringField(TEXT("message"), FString::Printf(TEXT("Wrap mode set to %s"), *WrapMode));
        return Response;
    }
    
    if (SubAction == TEXT("create_render_target"))
    {
        FString Name = GetStringFieldTextAuth(Params, TEXT("name"), TEXT(""));
        FString Path = NormalizeContentAssetPath(GetStringFieldTextAuth(Params, TEXT("path"), TEXT("/Game/Textures")));
        
        // Support renderTargetPath as alternative to name+path
        FString RenderTargetPath = GetStringFieldTextAuth(Params, TEXT("renderTargetPath"), TEXT(""));
        if (!RenderTargetPath.IsEmpty())
        {
            // Extract name and path from renderTargetPath (e.g., "/Game/MCPTest/RT_Test" -> name="RT_Test", path="/Game/MCPTest")
            RenderTargetPath = NormalizeContentAssetPath(RenderTargetPath);
            int32 LastSlashIndex;
            if (RenderTargetPath.FindLastChar(TEXT('/'), LastSlashIndex))
            {
                Name = RenderTargetPath.RightChop(LastSlashIndex + 1);
                Path = RenderTargetPath.Left(LastSlashIndex);
            }
            else
            {
                Name = RenderTargetPath;
            }
        }
        
        int32 Width = static_cast<int32>(GetNumberFieldTextAuth(Params, TEXT("width"), 1024));
        int32 Height = static_cast<int32>(GetNumberFieldTextAuth(Params, TEXT("height"), 1024));
        
        if (Name.IsEmpty())
        {
            TEXTURE_ERROR_RESPONSE(TEXT("name is required"));
        }
        
        // Validated before the existence probe, so nothing downstream ever sees an unchecked
        // path. Both halves are caller text - and on the renderTargetPath branch above they are
        // a split of one caller string, so a "//" in it lands in `Path`. CreatePackage
        // (UObjectGlobals.cpp:1087-1120) logs at Fatal on that, which ends the editor PROCESS
        // rather than failing the call (B-createpackage-unvalidated-paths-plugin-wide).
        // No trailing-slash trim is needed on Path: NormalizeContentAssetPath strips them at read
        // time, and the renderTargetPath split above cuts at the last '/' so it never leaves one.
        FString FullPath;
        FString PathError;
        if (!PinWrightComposeAssetPackagePath(Path, Name, FullPath, PathError))
        {
            Response->SetStringField(TEXT("errorCode"), TEXT("INVALID_ARGUMENT"));
            TEXTURE_ERROR_RESPONSE(FString::Printf(
                TEXT("%s Pass a bare asset name in 'name' and choose the folder with 'path', or "
                     "one combined 'renderTargetPath'."), *PathError));
        }

        // Registry-only existence check (no load side effect); a StaticLoadObject +
        // FindPackage probe would orphan an empty UPackage and false-positive on every
        // brand-new path. See B-create-render-target-false-collision / NewPathSucceeds test.
        if (ResolveAsset(FullPath).bExists)
        {
            TEXTURE_ERROR_RESPONSE(FString::Printf(TEXT("Asset with this name already exists: %s"), *FullPath));
        }

        // Create package first
        UPackage* Package = CreatePackage(*FullPath);
        if (!Package)
        {
            TEXTURE_ERROR_RESPONSE(TEXT("Failed to create package"));
        }
        
        // Create render target directly in the package
        UTextureRenderTarget2D* RenderTarget = NewObject<UTextureRenderTarget2D>(Package, UTextureRenderTarget2D::StaticClass(), FName(*Name), RF_Public | RF_Standalone);
        if (!RenderTarget)
        {
            TEXTURE_ERROR_RESPONSE(TEXT("Failed to create render target"));
        }
        
        RenderTarget->InitCustomFormat(Width, Height, PF_B8G8R8A8, true);
        
        McpSaveTextureToDisk(Response, RenderTarget, /*bSave=*/true);
        
        Response->SetBoolField(TEXT("success"), true);
        Response->SetStringField(TEXT("message"), FString::Printf(TEXT("Render target '%s' created"), *Name));
        Response->SetStringField(TEXT("assetPath"), FullPath);
        return Response;
    }
    
    // Unknown action
    Response->SetBoolField(TEXT("success"), false);
    Response->SetStringField(TEXT("error"), FString::Printf(TEXT("Unknown texture action: %s"), *SubAction));
    return Response;
}

static bool RunTextureAction(FHandlerContext& Ctx, const FString& SubAction)
{
    TSharedPtr<FJsonObject> Params = Ctx.GetRawPayload().IsValid()
        ? MakeShared<FJsonObject>(*Ctx.GetRawPayload())
        : MakeShared<FJsonObject>();

    Params->SetStringField(TEXT("subAction"), SubAction);

    TSharedPtr<FJsonObject> Result = ExecuteTextureAction(Params);
    if (!Result.IsValid())
    {
        Ctx.SendError(TEXT("TEXTURE_ERROR"), TEXT("Texture action returned no result."));
        return true;
    }

    const bool bSuccess = GetJsonBoolField(Result, TEXT("success"), false);
    if (bSuccess)
    {
        Result->RemoveField(TEXT("success"));
        Ctx.SendSuccess(Result);
    }
    else
    {
        const FString Error = GetJsonStringField(Result, TEXT("error"), TEXT("Texture action failed"));
        const FString ErrorCode = GetJsonStringField(Result, TEXT("errorCode"), TEXT("TEXTURE_ERROR"));
        Ctx.SendError(ErrorCode, Error);
    }
    return true;
}

#define REGISTER_TEXTURE_ACTION_HANDLER(Endpoint, SubActionName, SummaryText, ...) \
REGISTER_RPC_HANDLER(Endpoint, "Texture", SummaryText, __VA_ARGS__) \
{ \
    return RunTextureAction(Ctx, TEXT(SubActionName)); \
}

REGISTER_TEXTURE_ACTION_HANDLER("texture.create_noise_texture", "create_noise_texture", "Create a procedural FBM noise texture (Perlin value noise or Worley/Voronoi cellular)",
    RPC_PARAMS(
        RPC_PARAM_REQ("name", "string", "Asset name for the new texture"),
        RPC_PARAM_DEF("path", "path", "Destination package path", "/Game/Textures"),
        RPC_PARAM_DEF("noiseType", "string", "Noise algorithm: Perlin (smoothed value noise) or Worley / Voronoi (cellular F1 distance, dark at cell centres). Any other name is refused; the resolved algorithm is echoed back as noiseType", "Perlin"),
        RPC_PARAM_DEF("width", "integer", "Texture width in pixels", "1024"),
        RPC_PARAM_DEF("height", "integer", "Texture height in pixels", "1024"),
        RPC_PARAM_DEF("scale", "number", "Noise frequency, in lattice cells across the texture. Rounded to a whole number when seamless is set (which needs at least 2 cells for the base octave to vary); the value used is echoed back as effectiveScale", "1.0"),
        RPC_PARAM_DEF("octaves", "integer", "Number of FBM octaves. Must be between 1 and 16; 0 is refused rather than read as flat, because an FBM with no octaves has nothing to normalise by", "4"),
        RPC_PARAM_DEF("persistence", "number", "Amplitude falloff per octave. Must be between 0 and 1; a negative persistence is refused because it cancels the octave amplitudes to zero, leaving the FBM nothing to normalise by", "0.5"),
        RPC_PARAM_DEF("lacunarity", "number", "Frequency growth per octave", "2.0"),
        RPC_PARAM_DEF("seed", "integer", "Random seed", "0"),
        RPC_PARAM_DEF("seamless", "boolean", "Tile exactly by wrapping the noise lattice at the texture edge, so the image repeats with no seam; snaps scale (and each octave) to a whole cell count", "false"),
        RPC_PARAM_DEF("hdr", "boolean", "Create an HDR texture: a linear RGBA16F source written as half-floats, instead of the sRGB BGRA8 default. Echoed back as hdr", "false"),
        RPC_PARAM_DEF("save", "boolean", "Save the asset to disk", "true")))
REGISTER_TEXTURE_ACTION_HANDLER("texture.create_gradient_texture", "create_gradient_texture", "Create a procedural gradient texture",
    RPC_PARAMS(
        RPC_PARAM_REQ("name", "string", "Asset name for the new texture"),
        RPC_PARAM_DEF("path", "path", "Destination package path", "/Game/Textures"),
        RPC_PARAM_DEF("gradientType", "string", "Gradient type (Linear, Radial, Angular)", "Linear"),
        RPC_PARAM_DEF("width", "integer", "Texture width in pixels", "1024"),
        RPC_PARAM_DEF("height", "integer", "Texture height in pixels", "1024"),
        RPC_PARAM_DEF("angle", "number", "Gradient angle in degrees (Linear)", "0.0"),
        RPC_PARAM_DEF("centerX", "number", "Gradient center X (0-1)", "0.5"),
        RPC_PARAM_DEF("centerY", "number", "Gradient center Y (0-1)", "0.5"),
        RPC_PARAM_DEF("radius", "number", "Gradient radius (Radial)", "0.5"),
        RPC_PARAM_DEF("hdr", "boolean", "Create an HDR (float) texture", "false"),
        RPC_PARAM_DEF("save", "boolean", "Save the asset to disk", "true"),
        RPC_PARAM_OPT("startColor", "object", "Gradient start color {r,g,b,a}"),
        RPC_PARAM_OPT("endColor", "object", "Gradient end color {r,g,b,a}")))
REGISTER_TEXTURE_ACTION_HANDLER("texture.create_pattern_texture", "create_pattern_texture", "Create a procedural pattern texture",
    RPC_PARAMS(
        RPC_PARAM_REQ("name", "string", "Asset name for the new texture"),
        RPC_PARAM_DEF("path", "path", "Destination package path", "/Game/Textures"),
        RPC_PARAM_DEF("patternType", "string", "Pattern (Checker, Grid, Brick, Stripes, Dots)", "Checker"),
        RPC_PARAM_DEF("width", "integer", "Texture width in pixels", "1024"),
        RPC_PARAM_DEF("height", "integer", "Texture height in pixels", "1024"),
        RPC_PARAM_DEF("tilesX", "integer", "Pattern tiles along X", "8"),
        RPC_PARAM_DEF("tilesY", "integer", "Pattern tiles along Y", "8"),
        RPC_PARAM_DEF("lineWidth", "number", "Grid/brick line width (0-1)", "0.02"),
        RPC_PARAM_DEF("brickRatio", "number", "Brick width-to-height ratio", "2.0"),
        RPC_PARAM_DEF("offset", "number", "Per-row brick offset", "0.5"),
        RPC_PARAM_DEF("save", "boolean", "Save the asset to disk", "true"),
        RPC_PARAM_OPT("primaryColor", "object", "Primary pattern color {r,g,b,a}"),
        RPC_PARAM_OPT("secondaryColor", "object", "Secondary pattern color {r,g,b,a}")))
REGISTER_TEXTURE_ACTION_HANDLER("texture.create_normal_from_height", "create_normal_from_height", "Generate a normal map from height",
    RPC_PARAMS(
        RPC_PARAM_REQ("sourceTexture", "path", "Height map texture asset path"),
        RPC_PARAM_OPT("name", "string", "Output asset name (defaults to <source>_N)"),
        RPC_PARAM_OPT("path", "path", "Output package path (defaults to source folder)"),
        RPC_PARAM_DEF("strength", "number", "Normal strength multiplier", "1.0"),
        RPC_PARAM_DEF("algorithm", "string", "Gradient algorithm (Sobel or finite-difference)", "Sobel"),
        RPC_PARAM_DEF("flipY", "boolean", "Flip the Y component (DirectX vs OpenGL)", "false"),
        RPC_PARAM_DEF("save", "boolean", "Save the asset to disk", "true"),
        RPC_PARAM_DEF("channelMode", "string", "Height channel source (luminance, red, green, blue, alpha, average)", "luminance")))
REGISTER_TEXTURE_ACTION_HANDLER("texture.set_compression_settings", "set_compression_settings", "Set texture compression settings",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath", "path", "Texture asset path"),
        RPC_PARAM_DEF("compressionSettings", "string", "Compression mode (e.g. TC_Default, TC_Normalmap, TC_Masks)", "TC_Default"),
        RPC_PARAM_DEF("save", "boolean", "Save the asset to disk", "true")))
REGISTER_TEXTURE_ACTION_HANDLER("texture.set_texture_group", "set_texture_group", "Set texture group",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath", "path", "Texture asset path"),
        RPC_PARAM_DEF("textureGroup", "string", "LOD/texture group (e.g. TEXTUREGROUP_World, Character, UI)", "TEXTUREGROUP_World"),
        RPC_PARAM_DEF("save", "boolean", "Save the asset to disk", "true")))
REGISTER_TEXTURE_ACTION_HANDLER("texture.set_lod_bias", "set_lod_bias", "Set texture LOD bias",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath", "path", "Texture asset path"),
        RPC_PARAM_DEF("lodBias", "number", "LOD bias value", "0"),
        RPC_PARAM_DEF("save", "boolean", "Save the asset to disk", "true")))
REGISTER_TEXTURE_ACTION_HANDLER("texture.configure_virtual_texture", "configure_virtual_texture", "Configure VT settings",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath", "path", "Texture asset path"),
        RPC_PARAM_DEF("virtualTextureStreaming", "boolean", "Enable virtual texture streaming", "false"),
        RPC_PARAM_DEF("save", "boolean", "Save the asset to disk", "true")))
REGISTER_TEXTURE_ACTION_HANDLER("texture.set_streaming_priority", "set_streaming_priority", "Set streaming priority",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath", "path", "Texture asset path"),
        RPC_PARAM_DEF("neverStream", "boolean", "Disable mip streaming for this texture", "false"),
        RPC_PARAM_DEF("save", "boolean", "Save the asset to disk", "true")))
REGISTER_TEXTURE_ACTION_HANDLER("texture.get_texture_info", "get_texture_info", "Get texture info",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath", "path", "Texture asset path")))
REGISTER_RPC_HANDLER("texture.describe", "Texture", "Describe any UTexture subclass using the texture dump JSON shape",
    RPC_PARAMS(RPC_PARAM_REQ("assetPath", "path", "Texture asset path")))
{
    FString AssetPath;
    if (!Ctx.RequireAssetPath(TEXT("assetPath"), AssetPath))
    {
        return true;
    }

    UTexture* Texture = LoadObject<UTexture>(nullptr, *AssetPath);
    if (!Texture)
    {
        Ctx.SendError(TEXT("ASSET_NOT_FOUND"),
            FString::Printf(TEXT("Could not load texture: %s"), *AssetPath));
        return true;
    }

    if (TextureDumpBuilder::IsCompiling(Texture))
    {
        Ctx.SendError(TEXT("ASSET_COMPILING"),
            FString::Printf(
                TEXT("Texture '%s' is still compiling platform data. Retry texture.describe after compilation finishes."),
                *AssetPath));
        return true;
    }

    Ctx.SendSuccess(TextureDumpBuilder::BuildTextureJson(Texture));
    return true;
}
REGISTER_RPC_HANDLER("texture.get_pixel_stats", "Texture",
    "Read live source-mip pixel content (per-channel mean/min/max, grayscale flag, content hash) so the pixel-mutating verbs (desaturate/invert/adjust_levels/...) are verifiable, not just inferred from sizeBytes. Pass region and/or tileGrid to measure a sub-rectangle or a columns x rows tiling of one, which is what a tiled authoring texture (SubUV flipbook atlas, sprite sheet, row-banded mask sheet) needs - its whole-image mean says nothing about any cell",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath", "path", "Texture asset path"),
        RPC_PARAM_DEF("mip", "integer", "Source mip level to read (0 = full-res)", "0"),
        RPC_PARAM_OPT("region", "object", "Measure a sub-rectangle instead of the whole mip: {x, y, width, height} in that mip's pixels. x/y default to 0; the rect is clamped to the mip and echoed back as 'region'"),
        RPC_PARAM_OPT("tileGrid", "object", "Split the measured area into {columns, rows} tiles and report one stats block per tile in 'tiles', row-major. Combine with 'region' to tile a sub-rectangle")))
{
    FString AssetPath;
    if (!Ctx.RequireAssetPath(TEXT("assetPath"), AssetPath))
    {
        return true;
    }

    UTexture* Texture = LoadObject<UTexture>(nullptr, *AssetPath);
    if (!Texture)
    {
        Ctx.SendError(TEXT("ASSET_NOT_FOUND"),
            FString::Printf(TEXT("Could not load texture: %s"), *AssetPath));
        return true;
    }

    TexturePixelStats::FPixelStatsOptions Options;
    Options.MipIndex = Ctx.GetInt(TEXT("mip"), 0);

    if (const TSharedPtr<FJsonObject> RegionObj = Ctx.GetObject(TEXT("region")))
    {
        double RegionWidth = 0.0;
        double RegionHeight = 0.0;
        if (!RegionObj->TryGetNumberField(TEXT("width"), RegionWidth)
            || !RegionObj->TryGetNumberField(TEXT("height"), RegionHeight))
        {
            Ctx.SendError(TEXT("INVALID_ARGUMENT"),
                TEXT("'region' needs numeric 'width' and 'height' in mip pixels ('x' and 'y' default to 0)."));
            return true;
        }
        double RegionX = 0.0;
        double RegionY = 0.0;
        RegionObj->TryGetNumberField(TEXT("x"), RegionX);
        RegionObj->TryGetNumberField(TEXT("y"), RegionY);

        Options.bHasRegion = true;
        Options.Region.X = FMath::TruncToInt32(RegionX);
        Options.Region.Y = FMath::TruncToInt32(RegionY);
        Options.Region.Width = FMath::TruncToInt32(RegionWidth);
        Options.Region.Height = FMath::TruncToInt32(RegionHeight);
    }

    if (const TSharedPtr<FJsonObject> GridObj = Ctx.GetObject(TEXT("tileGrid")))
    {
        double Columns = 0.0;
        double Rows = 0.0;
        if (!GridObj->TryGetNumberField(TEXT("columns"), Columns)
            || !GridObj->TryGetNumberField(TEXT("rows"), Rows))
        {
            Ctx.SendError(TEXT("INVALID_ARGUMENT"),
                TEXT("'tileGrid' needs numeric 'columns' and 'rows'."));
            return true;
        }
        Options.bHasTileGrid = true;
        Options.TileColumns = FMath::TruncToInt32(Columns);
        Options.TileRows = FMath::TruncToInt32(Rows);
    }

    FString Error;
    TSharedPtr<FJsonObject> Stats = TexturePixelStats::BuildPixelStatsJson(Texture, Options, Error);
    if (!Stats.IsValid())
    {
        Ctx.SendError(TEXT("PIXEL_STATS_UNAVAILABLE"), Error);
        return true;
    }

    Stats->SetStringField(TEXT("assetPath"), AssetPath);
    Ctx.SendSuccess(Stats);
    return true;
}
REGISTER_TEXTURE_ACTION_HANDLER("texture.resize_texture", "resize_texture", "Resize texture",
    RPC_PARAMS(
        RPC_PARAM_REQ("sourcePath", "path", "Source texture asset path"),
        RPC_PARAM_OPT("name", "string", "Output asset name (defaults to <source>_Resized)"),
        RPC_PARAM_OPT("path", "path", "Output package path (defaults to source folder)"),
        RPC_PARAM_DEF("newWidth", "integer", "Target width in pixels", "512"),
        RPC_PARAM_DEF("newHeight", "integer", "Target height in pixels", "512"),
        RPC_PARAM_DEF("save", "boolean", "Save the asset to disk", "true")))
REGISTER_TEXTURE_ACTION_HANDLER("texture.invert", "invert", "Invert texture",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath", "path", "Texture asset path"),
        RPC_PARAM_DEF("inPlace", "boolean", "Edit in place instead of creating a copy", "true"),
        RPC_PARAM_OPT("name", "string", "Output asset name when not in place"),
        RPC_PARAM_OPT("path", "path", "Output package path when not in place"),
        RPC_PARAM_DEF("save", "boolean", "Save the asset to disk", "true")))
REGISTER_TEXTURE_ACTION_HANDLER("texture.desaturate", "desaturate", "Desaturate texture",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath", "path", "Texture asset path"),
        RPC_PARAM_DEF("amount", "number", "Desaturation amount (0-1)", "1.0"),
        RPC_PARAM_DEF("inPlace", "boolean", "Edit in place instead of creating a copy", "true"),
        RPC_PARAM_OPT("name", "string", "Output asset name when not in place"),
        RPC_PARAM_OPT("path", "path", "Output package path when not in place"),
        RPC_PARAM_DEF("save", "boolean", "Save the asset to disk", "true")))
REGISTER_TEXTURE_ACTION_HANDLER("texture.adjust_levels", "adjust_levels", "Adjust levels",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath", "path", "Texture asset path"),
        RPC_PARAM_DEF("inBlack", "number", "Input black point (0-1)", "0.0"),
        RPC_PARAM_DEF("inWhite", "number", "Input white point (0-1)", "1.0"),
        RPC_PARAM_DEF("gamma", "number", "Gamma adjustment", "1.0"),
        RPC_PARAM_DEF("outBlack", "number", "Output black point (0-1)", "0.0"),
        RPC_PARAM_DEF("outWhite", "number", "Output white point (0-1)", "1.0"),
        RPC_PARAM_DEF("inPlace", "boolean", "Edit in place instead of creating a copy", "true"),
        RPC_PARAM_DEF("save", "boolean", "Save the asset to disk", "true")))
REGISTER_TEXTURE_ACTION_HANDLER("texture.blur", "blur", "Blur texture",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath", "path", "Texture asset path"),
        RPC_PARAM_DEF("radius", "integer", "Box blur radius in pixels (1-10)", "2"),
        RPC_PARAM_DEF("save", "boolean", "Save the asset to disk", "true")))
REGISTER_TEXTURE_ACTION_HANDLER("texture.sharpen", "sharpen", "Sharpen texture",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath", "path", "Texture asset path"),
        RPC_PARAM_DEF("amount", "number", "Sharpen amount (0-5)", "1.0"),
        RPC_PARAM_DEF("save", "boolean", "Save the asset to disk", "true")))
REGISTER_TEXTURE_ACTION_HANDLER("texture.channel_pack", "channel_pack", "Pack channels",
    RPC_PARAMS(
        RPC_PARAM_DEF("name", "string", "Output asset name", "ChannelPacked"),
        RPC_PARAM_OPT("redTexture", "path", "Source texture for the red channel"),
        RPC_PARAM_OPT("greenTexture", "path", "Source texture for the green channel"),
        RPC_PARAM_OPT("blueTexture", "path", "Source texture for the blue channel"),
        RPC_PARAM_OPT("alphaTexture", "path", "Source texture for the alpha channel"),
        RPC_PARAM_DEF("path", "path", "Destination package path", "/Game/Textures"),
        RPC_PARAM_DEF("save", "boolean", "Save the asset to disk", "true")))
REGISTER_TEXTURE_ACTION_HANDLER("texture.combine_textures", "combine_textures", "Combine textures",
    RPC_PARAMS(
        RPC_PARAM_REQ("baseTexture", "path", "Base texture asset path"),
        RPC_PARAM_REQ("overlayTexture", "path", "Overlay texture asset path"),
        RPC_PARAM_DEF("blendMode", "string", "Blend mode (Normal, Multiply, Screen, Overlay, Add)", "Normal"),
        RPC_PARAM_DEF("opacity", "number", "Overlay opacity (0-1)", "1.0"),
        RPC_PARAM_DEF("name", "string", "Output asset name", "Combined"),
        RPC_PARAM_DEF("path", "path", "Destination package path", "/Game/Textures"),
        RPC_PARAM_DEF("save", "boolean", "Save the asset to disk", "true")))
REGISTER_TEXTURE_ACTION_HANDLER("texture.adjust_curves", "adjust_curves", "Adjust color curves",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath", "path", "Texture asset path"),
        RPC_PARAM_DEF("inPlace", "boolean", "Edit in place instead of creating a copy", "true"),
        RPC_PARAM_OPT("name", "string", "Output asset name when not in place"),
        RPC_PARAM_OPT("path", "path", "Output package path when not in place"),
        RPC_PARAM_DEF("save", "boolean", "Save the asset to disk", "true"),
        RPC_PARAM_OPT("input", "array", "Master curve input control points (0-1)"),
        RPC_PARAM_OPT("output", "array", "Master curve output control points (0-1)"),
        RPC_PARAM_OPT("inputR", "array", "Red channel input control points (0-1)"),
        RPC_PARAM_OPT("outputR", "array", "Red channel output control points (0-1)"),
        RPC_PARAM_OPT("inputG", "array", "Green channel input control points (0-1)"),
        RPC_PARAM_OPT("outputG", "array", "Green channel output control points (0-1)"),
        RPC_PARAM_OPT("inputB", "array", "Blue channel input control points (0-1)"),
        RPC_PARAM_OPT("outputB", "array", "Blue channel output control points (0-1)")))
REGISTER_TEXTURE_ACTION_HANDLER("texture.channel_extract", "channel_extract", "Extract channel",
    RPC_PARAMS(
        RPC_PARAM_REQ("texturePath", "path", "Source texture asset path"),
        RPC_PARAM_DEF("channel", "string", "Channel to extract (R, G, B, A)", "R"),
        RPC_PARAM_OPT("outputPath", "path", "Output package path (defaults to source folder)"),
        RPC_PARAM_OPT("name", "string", "Output asset name (defaults to <source>_<channel>)"),
        RPC_PARAM_DEF("save", "boolean", "Save the asset to disk", "true")))
REGISTER_TEXTURE_ACTION_HANDLER("texture.set_texture_filter", "set_texture_filter", "Set filter",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath", "path", "Texture asset path"),
        RPC_PARAM_DEF("filter", "string", "Filter mode (Default, Nearest, Bilinear, Trilinear)", "Default"),
        RPC_PARAM_DEF("save", "boolean", "Save the asset to disk", "true")))
REGISTER_TEXTURE_ACTION_HANDLER("texture.set_texture_wrap", "set_texture_wrap", "Set wrap mode",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath", "path", "Texture asset path"),
        RPC_PARAM_DEF("wrapMode", "string", "Wrap mode (Wrap, Clamp, Mirror)", "Wrap"),
        RPC_PARAM_DEF("save", "boolean", "Save the asset to disk", "true")))
REGISTER_TEXTURE_ACTION_HANDLER("texture.create_render_target", "create_render_target", "Create render target",
    RPC_PARAMS(
        RPC_PARAM_OPT("name", "string", "Render target asset name (or supply renderTargetPath)"),
        RPC_PARAM_OPT("renderTargetPath", "path", "Full asset path; overrides name+path when set"),
        RPC_PARAM_DEF("path", "path", "Destination package path", "/Game/Textures"),
        RPC_PARAM_DEF("width", "integer", "Render target width in pixels", "1024"),
        RPC_PARAM_DEF("height", "integer", "Render target height in pixels", "1024")))

#undef REGISTER_TEXTURE_ACTION_HANDLER
#undef TEXTURE_ERROR_RESPONSE

#undef GetStringFieldTextAuth
#undef GetNumberFieldTextAuth
#undef GetBoolFieldTextAuth


