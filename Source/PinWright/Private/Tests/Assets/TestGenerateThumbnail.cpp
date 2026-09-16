// Copyright (c) 2026 Alexander Penkin. MIT License.

// Tests for asset.generate_thumbnail's image-format and alpha contracts, and for the preview
// override parameter validation.
//
// Two defects are pinned here:
//
//  1. FORMAT. The verb encoded through FImageUtils::ThumbnailCompressImageArray, which emits
//     JPEG for any image >= 8x8, and wrote those bytes to the caller's .png path while
//     reporting success — a file whose magic bytes are FF D8 FF E0 ... JFIF with a .png name.
//
//  2. ALPHA. The verb copied the render's alpha channel through untouched. The thumbnail path
//     clears its canvas to opaque black but does no alpha fix-up after the scene render, so
//     whatever the renderer left survives — the same shape as the ~99.97%-transparent-PNG
//     defect. The old JPEG encoding masked it only because JPEG has no alpha channel; fixing
//     the format without stamping alpha would have SHIPPED the transparency bug.
//
// The alpha assertion deliberately runs through the production encode entry point
// (PinWrightThumbnail::EncodeByExtension), which is the single function the handler calls and
// which stamps alpha itself. That is the lesson from the earlier alpha test that called
// PinWrightScreenshotUtils::ForceOpaqueAlpha directly and kept passing with the production call
// site deleted: a test that supplies the fix it is meant to verify proves nothing. Here the
// input bitmap is fully transparent and nothing in the test touches alpha, so the only way the
// assertion passes is if the shipped code path stamps it.
//
// Counterfactual for the format tests: route the handler back through
// ThumbnailCompressImageArray and the .png assertion fails on the JPEG signature.
// Counterfactual for the alpha test: delete the ForceOpaqueAlpha call inside
// EncodeByExtension and every decoded pixel comes back alpha 0.
#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Tests/TestUtils.h"
#include "Handlers/Asset/ThumbnailEncodeUtils.h"
#include "Handlers/Asset/ThumbnailFrameEvidence.h"
#include "Handlers/Asset/ThumbnailPreviewOverride.h"
#include "Compat/EngineVersionCompat.h"
#include "Tests/TestSkipReporting.h"

#include "ImageUtils.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "HAL/FileManager.h"
#include "UObject/Package.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Factories/MaterialInstanceConstantFactoryNew.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
// FBoolProperty / CastField, used to arm a usage flag whose direct member access is deprecated.
#include "UObject/UnrealType.h"

namespace
{
    // Prefixed: the plugin builds with Unity on, so file-local helpers must not collide with a
    // sibling TU's anonymous namespace.
    const uint8 PWThumbPngSignature[8] = { 0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A };

    bool PWThumbStartsWithPngSignature(const TArray<uint8>& Bytes)
    {
        if (Bytes.Num() < 8)
        {
            return false;
        }
        for (int32 Index = 0; Index < 8; ++Index)
        {
            if (Bytes[Index] != PWThumbPngSignature[Index])
            {
                return false;
            }
        }
        return true;
    }

    // JPEG SOI + APP0 — the first two bytes of the JFIF files the old encoder produced.
    bool PWThumbStartsWithJpegSoi(const TArray<uint8>& Bytes)
    {
        return Bytes.Num() >= 2 && Bytes[0] == 0xFF && Bytes[1] == 0xD8;
    }

    // A bitmap whose every pixel is FULLY TRANSPARENT with distinct, recoverable RGB. If the
    // encode path does not stamp alpha, the decoded image is 100% transparent.
    TArray<FColor> PWThumbMakeTransparentBitmap(int32 Width, int32 Height)
    {
        TArray<FColor> Bitmap;
        Bitmap.Reserve(Width * Height);
        for (int32 Index = 0; Index < Width * Height; ++Index)
        {
            Bitmap.Add(FColor(10, 120, 230, 0));
        }
        return Bitmap;
    }

    // Arms the Niagara mesh-particle usage flag, one of the three the engine's force-plane rule
    // reads. Set by reflection: direct member access is UE_DEPRECATED(5.8) in favour of an
    // accessor whose setter kicks off a shader recompile, and the gate reads the UPROPERTY either
    // way. Returns false if the engine no longer carries the field under this name.
    bool PWThumbArmNiagaraMeshUsage(UMaterial* Material)
    {
        FBoolProperty* Prop = CastField<FBoolProperty>(
            UMaterial::StaticClass()->FindPropertyByName(TEXT("bUsedWithNiagaraMeshParticles")));
        if (!Prop || !Material)
        {
            return false;
        }
        Prop->SetPropertyValue_InContainer(Material, true);
        return true;
    }

    // A synthetic master UMaterial plus one plain UMaterialInstanceConstant of it, under a
    // disposable package path. Deliberately content-free: the defect is about which object the
    // engine reads a flag off, not about what the material draws.
    bool PWThumbMakeMasterAndInstance(FAutomationTestBase& Test, const TCHAR* NameSuffix,
        FString& OutMasterPath, FString& OutInstancePath,
        UMaterial*& OutMaster, UMaterialInstanceConstant*& OutInstance)
    {
        OutMaster = nullptr;
        OutInstance = nullptr;
        const FString Unique = FGuid::NewGuid().ToString(EGuidFormats::Digits);
        OutMasterPath = FString::Printf(TEXT("/Game/__PW_GatewayTests/PWThumbM%s_%s"), NameSuffix, *Unique);
        OutInstancePath = FString::Printf(TEXT("/Game/__PW_GatewayTests/PWThumbI%s_%s"), NameSuffix, *Unique);

        UPackage* MasterPkg = CreatePackage(*OutMasterPath);
        if (!Test.TestNotNull(TEXT("master package created"), MasterPkg))
        {
            return false;
        }
        UMaterial* Master = NewObject<UMaterial>(MasterPkg,
            FName(*FPackageName::GetLongPackageAssetName(OutMasterPath)),
            RF_Public | RF_Standalone);
        if (!Test.TestNotNull(TEXT("master material created"), Master))
        {
            CleanupTestAsset(OutMasterPath);
            return false;
        }
        FAssetRegistryModule::AssetCreated(Master);

        UPackage* InstancePkg = CreatePackage(*OutInstancePath);
        if (!Test.TestNotNull(TEXT("instance package created"), InstancePkg))
        {
            CleanupTestAsset(OutMasterPath);
            return false;
        }
        UMaterialInstanceConstantFactoryNew* Factory = NewObject<UMaterialInstanceConstantFactoryNew>();
        Factory->InitialParent = Master;
        UMaterialInstanceConstant* Instance = Cast<UMaterialInstanceConstant>(
            Factory->FactoryCreateNew(UMaterialInstanceConstant::StaticClass(), InstancePkg,
                FName(*FPackageName::GetLongPackageAssetName(OutInstancePath)),
                RF_Public | RF_Standalone, nullptr, GWarn));
        if (!Test.TestNotNull(TEXT("material instance created"), Instance))
        {
            CleanupTestAsset(OutInstancePath);
            CleanupTestAsset(OutMasterPath);
            return false;
        }
        FAssetRegistryModule::AssetCreated(Instance);

        OutMaster = Master;
        OutInstance = Instance;
        return true;
    }

    // How much of the frame the preview shape covers, as the fraction of pixels differing from
    // the frame's own corner pixel. The thumbnail scene draws the primitive over a uniform
    // cleared background, so an edge-on plane scores a percent or two while a framed one covers
    // most of the view. Returns -1 when the image could not be measured.
    double PWThumbMeasureCoverage(const FString& PngPath)
    {
        FImage Loaded;
        if (!FImageUtils::LoadImage(*PngPath, Loaded))
        {
            return -1.0;
        }
        Loaded.ChangeFormat(ERawImageFormat::BGRA8, EGammaSpace::sRGB);
        TArrayView64<FColor> Pixels = Loaded.AsBGRA8();
        if (Pixels.Num() <= 0)
        {
            return -1.0;
        }

        const FColor Background = Pixels[0];
        int64 Differing = 0;
        for (const FColor& Pixel : Pixels)
        {
            const int32 Delta = FMath::Max3(
                FMath::Abs(static_cast<int32>(Pixel.R) - static_cast<int32>(Background.R)),
                FMath::Abs(static_cast<int32>(Pixel.G) - static_cast<int32>(Background.G)),
                FMath::Abs(static_cast<int32>(Pixel.B) - static_cast<int32>(Background.B)));
            if (Delta > 16)
            {
                ++Differing;
            }
        }
        return static_cast<double>(Differing) / static_cast<double>(Pixels.Num());
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGenerateThumbnailEncodesByExtensionTest,
    "PinWright.asset.generate_thumbnail.EncodesByExtension",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGenerateThumbnailEncodesByExtensionTest::RunTest(const FString& Parameters)
{
    constexpr int32 Width = 16;
    constexpr int32 Height = 16;

    // .png -> real PNG bytes, not JPEG wearing a .png name.
    {
        TArray<FColor> Bitmap = PWThumbMakeTransparentBitmap(Width, Height);
        TArray<uint8> Bytes;
        FString Format;
        const bool bEncoded = PinWrightThumbnail::EncodeByExtension(
            TEXT("C:/tmp/PinWrightThumbTest.png"), Width, Height, Bitmap, Bytes, Format);
        TestTrue(TEXT("a .png request encodes"), bEncoded);
        TestEqual(TEXT("a .png request reports format png"), Format, FString(TEXT("png")));
        TestTrue(TEXT("a .png request carries the PNG signature"), PWThumbStartsWithPngSignature(Bytes));
        TestFalse(TEXT("a .png request is NOT a JPEG"), PWThumbStartsWithJpegSoi(Bytes));
    }

    // .jpg / .jpeg -> JPEG, because the caller asked for it by extension.
    for (const TCHAR* JpegPath : { TEXT("C:/tmp/PinWrightThumbTest.jpg"), TEXT("C:/tmp/PinWrightThumbTest.JPEG") })
    {
        TArray<FColor> Bitmap = PWThumbMakeTransparentBitmap(Width, Height);
        TArray<uint8> Bytes;
        FString Format;
        const bool bEncoded =
            PinWrightThumbnail::EncodeByExtension(JpegPath, Width, Height, Bitmap, Bytes, Format);
        TestTrue(TEXT("a .jpg/.jpeg request encodes"), bEncoded);
        TestEqual(TEXT("a .jpg/.jpeg request reports format jpeg"), Format, FString(TEXT("jpeg")));
        TestTrue(TEXT("a .jpg/.jpeg request carries the JPEG SOI"), PWThumbStartsWithJpegSoi(Bytes));
    }

    // No extension, and an unrecognised one, both default to PNG: lossless is what a
    // thumbnail/contact-sheet workflow wants, and defaulting beats rejecting a forgotten suffix.
    for (const TCHAR* OtherPath : { TEXT("C:/tmp/PinWrightThumbTest"), TEXT("C:/tmp/PinWrightThumbTest.bmp") })
    {
        TArray<FColor> Bitmap = PWThumbMakeTransparentBitmap(Width, Height);
        TArray<uint8> Bytes;
        FString Format;
        PinWrightThumbnail::EncodeByExtension(OtherPath, Width, Height, Bitmap, Bytes, Format);
        TestEqual(TEXT("an unknown/absent extension defaults to png"), Format, FString(TEXT("png")));
        TestTrue(TEXT("the default encode carries the PNG signature"), PWThumbStartsWithPngSignature(Bytes));
    }

    // A degenerate bitmap is rejected rather than fed short data to the encoder.
    {
        TArray<FColor> TooSmall = PWThumbMakeTransparentBitmap(2, 2);
        TArray<uint8> Bytes;
        FString Format;
        TestFalse(TEXT("a bitmap smaller than Width*Height is rejected"),
            PinWrightThumbnail::EncodeByExtension(TEXT("C:/tmp/PinWrightThumbTest.png"),
                Width, Height, TooSmall, Bytes, Format));
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGenerateThumbnailEncodeStampsAlphaTest,
    "PinWright.asset.generate_thumbnail.EncodedImageIsOpaque",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGenerateThumbnailEncodeStampsAlphaTest::RunTest(const FString& Parameters)
{
    constexpr int32 Width = 16;
    constexpr int32 Height = 16;

    // Fully transparent input. The test never touches alpha itself — see the file header on why
    // that matters.
    TArray<FColor> Bitmap = PWThumbMakeTransparentBitmap(Width, Height);
    TArray<uint8> Bytes;
    FString Format;

    const FString TempPath = FPaths::Combine(FPaths::ProjectSavedDir(),
        TEXT("PinWright"), TEXT("Tests"), TEXT("ThumbnailOpaque.png"));

    const bool bEncoded =
        PinWrightThumbnail::EncodeByExtension(TempPath, Width, Height, Bitmap, Bytes, Format);
    TestTrue(TEXT("the production encode path succeeds"), bEncoded);
    if (!bEncoded)
    {
        return true;
    }

    // The in-memory bitmap must have been stamped in place too, because the handler hands the
    // same buffer to the encoder and nothing else re-reads it.
    bool bBitmapOpaque = true;
    bool bBitmapColorPreserved = true;
    for (const FColor& Pixel : Bitmap)
    {
        bBitmapOpaque &= (Pixel.A == 255);
        bBitmapColorPreserved &= (Pixel.R == 10 && Pixel.G == 120 && Pixel.B == 230);
    }
    TestTrue(TEXT("the encoder stamps the bitmap opaque"), bBitmapOpaque);
    TestTrue(TEXT("the encoder leaves RGB untouched"), bBitmapColorPreserved);

    // And the encoded file decodes back fully opaque. This is the assertion that would have
    // caught the shipped defect: a transparent PNG looks like a blank frame to any consumer
    // that composites alpha, while one that re-encodes without alpha shows it fine — so the
    // bug reads as "the renderer produced nothing".
    if (!FFileHelper::SaveArrayToFile(Bytes, *TempPath))
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("temp-png-unwritable"),
            TEXT("Skipped decode check: could not write the temp PNG."));
        return true;
    }
    ON_SCOPE_EXIT { IFileManager::Get().Delete(*TempPath, false, true, true); };

    FImage Loaded;
    if (!FImageUtils::LoadImage(*TempPath, Loaded))
    {
        AddError(TEXT("the encoded PNG does not decode"));
        return true;
    }
    Loaded.ChangeFormat(ERawImageFormat::BGRA8, EGammaSpace::sRGB);
    TArrayView64<FColor> Pixels = Loaded.AsBGRA8();

    int64 NonOpaque = 0;
    for (const FColor& Pixel : Pixels)
    {
        if (Pixel.A != 255)
        {
            ++NonOpaque;
        }
    }
    TestTrue(TEXT("the decoded PNG has pixels"), Pixels.Num() > 0);
    TestEqual(
        FString::Printf(TEXT("no pixel of the encoded thumbnail is non-opaque (%lld of %lld were)"),
            NonOpaque, static_cast<int64>(Pixels.Num())),
        NonOpaque, static_cast<int64>(0));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGenerateThumbnailPrimitiveNamesTest,
    "PinWright.asset.generate_thumbnail.PrimitiveNames",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGenerateThumbnailPrimitiveNamesTest::RunTest(const FString& Parameters)
{
    uint8 PrimitiveType = 0xFF;
    bool bCustomMesh = false;

    // The shapes the engine's thumbnail primitive enum offers, matched case-insensitively.
    // TPT_ShaderBall joined EThumbnailPrimType in UE 5.8; before that the spelling names nothing
    // the engine can render and the parser refuses it like any other unknown shape.
    const TCHAR* Names[] = { TEXT("sphere"), TEXT("CUBE"), TEXT("Plane"), TEXT("cylinder"),
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 8, 0)
        TEXT("shaderball"),
#endif
    };
    for (const TCHAR* Name : Names)
    {
        TestTrue(FString::Printf(TEXT("'%s' is a known primitive"), Name),
            PinWrightThumbnail::ParsePrimitiveName(Name, PrimitiveType, bCustomMesh));
        TestFalse(TEXT("a named shape is not the custom-mesh spelling"), bCustomMesh);
    }

    TestTrue(TEXT("'mesh' is a known primitive"),
        PinWrightThumbnail::ParsePrimitiveName(TEXT("mesh"), PrimitiveType, bCustomMesh));
    TestTrue(TEXT("'mesh' is flagged as needing a primitiveMesh path"), bCustomMesh);

    // 'none' is deliberately not exposed: on its own it renders a bare plane, which would read
    // to a caller as "no primitive applied" rather than as a shape choice.
    TestFalse(TEXT("'none' is not an accepted primitive"),
        PinWrightThumbnail::ParsePrimitiveName(TEXT("none"), PrimitiveType, bCustomMesh));
    TestFalse(TEXT("an unknown primitive name is rejected"),
        PinWrightThumbnail::ParsePrimitiveName(TEXT("torus"), PrimitiveType, bCustomMesh));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGenerateThumbnailParamValidationTest,
    "PinWright.asset.generate_thumbnail.ParamValidation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGenerateThumbnailParamValidationTest::RunTest(const FString& Parameters)
{
    // An unknown primitive is rejected before anything is loaded or rendered.
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), TEXT("/Engine/EngineMeshes/Cube"));
        Payload->SetStringField(TEXT("primitive"), TEXT("torus"));

        FTestResponseCapture Capture;
        TestTrue(TEXT("asset.generate_thumbnail handler found"),
            InvokeHandlerWithCapture(TEXT("asset.generate_thumbnail"), Payload, Capture));
        TestFalse(TEXT("an unknown primitive is rejected"), Capture.bSuccess);
        TestEqual(TEXT("unknown primitive error is typed"), Capture.ErrorCode,
            FString(TEXT("INVALID_ARGUMENT")));
    }

    // primitive:'mesh' without primitiveMesh would silently render a flat plane, so it is a
    // typed rejection rather than a wrong picture.
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), TEXT("/Engine/EngineMeshes/Cube"));
        Payload->SetStringField(TEXT("primitive"), TEXT("mesh"));

        FTestResponseCapture Capture;
        InvokeHandlerWithCapture(TEXT("asset.generate_thumbnail"), Payload, Capture);
        TestFalse(TEXT("primitive 'mesh' without primitiveMesh is rejected"), Capture.bSuccess);
        TestEqual(TEXT("missing primitiveMesh error is typed"), Capture.ErrorCode,
            FString(TEXT("INVALID_ARGUMENT")));
    }

    // primitiveMesh without primitive would be silently ignored otherwise.
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), TEXT("/Engine/EngineMeshes/Cube"));
        Payload->SetStringField(TEXT("primitiveMesh"), TEXT("/Engine/EngineMeshes/Sphere"));

        FTestResponseCapture Capture;
        InvokeHandlerWithCapture(TEXT("asset.generate_thumbnail"), Payload, Capture);
        TestFalse(TEXT("primitiveMesh without primitive is rejected"), Capture.bSuccess);
        TestEqual(TEXT("orphan primitiveMesh error is typed"), Capture.ErrorCode,
            FString(TEXT("INVALID_ARGUMENT")));
    }

    // A missing asset is still ASSET_NOT_FOUND, not a new code from the override path.
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), TEXT("/Game/PinWrightNoSuchAssetForThumbnailTest"));

        FTestResponseCapture Capture;
        InvokeHandlerWithCapture(TEXT("asset.generate_thumbnail"), Payload, Capture);
        TestFalse(TEXT("a missing asset is rejected"), Capture.bSuccess);
        TestEqual(TEXT("missing asset error is typed"), Capture.ErrorCode,
            FString(TEXT("ASSET_NOT_FOUND")));
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGenerateThumbnailWritesPngAndLeavesAssetCleanTest,
    "PinWright.asset.generate_thumbnail.WritesPngAndLeavesAssetClean",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGenerateThumbnailWritesPngAndLeavesAssetCleanTest::RunTest(const FString& Parameters)
{
    const FString AssetPath = TEXT("/Engine/EngineMeshes/Cube");
    UObject* Asset = LoadObject<UObject>(nullptr, *AssetPath);
    if (!Asset)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("engine-cube-unavailable"),
            TEXT("Skipped: /Engine/EngineMeshes/Cube unavailable in this run."));
        return true;
    }

    UPackage* Package = Asset->GetOutermost();
    const bool bDirtyBefore = Package && Package->IsDirty();

    const FString OutPath = FPaths::Combine(FPaths::ProjectSavedDir(),
        TEXT("PinWright"), TEXT("Tests"), TEXT("GenerateThumbnailHandler.png"));
    IFileManager::Get().Delete(*OutPath, false, true, true);

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), AssetPath);
    Payload->SetNumberField(TEXT("width"), 64);
    Payload->SetNumberField(TEXT("height"), 64);
    Payload->SetStringField(TEXT("outputPath"), OutPath);

    FTestResponseCapture Capture;
    TestTrue(TEXT("asset.generate_thumbnail handler found"),
        InvokeHandlerWithCapture(TEXT("asset.generate_thumbnail"), Payload, Capture));

    if (!Capture.bSuccess)
    {
        TestEqual(TEXT("failure is the typed render failure"), Capture.ErrorCode,
            FString(TEXT("THUMBNAIL_GENERATION_FAILED")));
        PinWrightTestSkip::SkipAssertions(*this, TEXT("thumbnail-render-unavailable"),
            TEXT("Skipped file checks: thumbnail rendering unavailable in this run (no RHI)."));
        return true;
    }
    ON_SCOPE_EXIT { IFileManager::Get().Delete(*OutPath, false, true, true); };

    // A read-only preview verb must not dirty the package it previews. The handler used to call
    // MarkPackageDirty() unconditionally, so an on-disk export left the asset needing a save.
    if (Package && !bDirtyBefore)
    {
        TestFalse(TEXT("generating a thumbnail does not dirty the asset package"), Package->IsDirty());
    }

    if (Capture.Result.IsValid())
    {
        FString Format;
        Capture.Result->TryGetStringField(TEXT("format"), Format);
        TestEqual(TEXT("a .png outputPath reports format png"), Format, FString(TEXT("png")));
    }

    TArray<uint8> FileBytes;
    if (!FFileHelper::LoadFileToArray(FileBytes, *OutPath))
    {
        AddError(TEXT("the reported thumbnail file could not be read back"));
        return true;
    }

    // The whole point of the fix: the bytes match the extension.
    TestTrue(TEXT("the .png file really is a PNG"), PWThumbStartsWithPngSignature(FileBytes));
    TestFalse(TEXT("the .png file is not JPEG/JFIF"), PWThumbStartsWithJpegSoi(FileBytes));

    // And it is opaque on the real handler path, not just in the unit test above.
    FImage Loaded;
    if (FImageUtils::LoadImage(*OutPath, Loaded))
    {
        Loaded.ChangeFormat(ERawImageFormat::BGRA8, EGammaSpace::sRGB);
        TArrayView64<FColor> Pixels = Loaded.AsBGRA8();
        int64 NonOpaque = 0;
        for (const FColor& Pixel : Pixels)
        {
            if (Pixel.A != 255)
            {
                ++NonOpaque;
            }
        }
        TestEqual(
            FString::Printf(TEXT("the written thumbnail is fully opaque (%lld of %lld were not)"),
                NonOpaque, static_cast<int64>(Pixels.Num())),
            NonOpaque, static_cast<int64>(0));
    }

    return true;
}

// ============================================================================
// B-thumbnail-cold-first-frame-no-stats: the frame evidence block
// ============================================================================
//
// THE DEFECT. asset.generate_thumbnail rendered once, with NeverFlush, and published NO
// measurement of the pixels: `success`, `assetPath`, `width`, `height` and (with an outputPath)
// `outputPath` / `format`. So the FIRST render after a compile — blank, or drawn with an
// unfinished shader — returned a payload byte-for-byte identical to the finished one. A caller
// could not tell them apart, and a cold t0 compared against a warm t1 produced a large measured
// difference that was entirely warm-up artefact; two motion proofs on one build were invalidated
// that way and had to be re-shot.
//
// COUNTERFACTUAL for both tests below. Delete the AddFrameEvidenceFields call from the handler
// (or the CalculateCaptureImageStats call that feeds it) and the cold response becomes identical
// to the warm one again: FrameEvidenceSeparatesColdFromReal fails at the cold-vs-warm comparison,
// and HandlerPublishesMeasuredFrameEvidence fails to find `imageStats` at all.
//
// Both assert on PIXELS, deliberately. A test that only checks a PNG was written passes on a
// blank frame, which is the exact failure being fixed.

namespace
{
    // A flat frame: every pixel the same colour, so exactly one 8-bit luminance level is
    // populated. Both cold frames measured in the ticket are this shape — an entirely empty
    // (black) frame, and a washed-out uniform grey one.
    TArray<FColor> PWThumbFlatFrame(int32 Width, int32 Height, FColor Value)
    {
        TArray<FColor> Frame;
        Frame.Init(Value, Width * Height);
        return Frame;
    }

    // A frame that was really drawn: a horizontal grey ramp over the full 8-bit range, so every
    // one of the 256 luminance levels carries Height pixels. 256 columns keeps the mapping from
    // column index to luminance level exact (R==G==B==i gives level i).
    TArray<FColor> PWThumbRampFrame(int32 Height)
    {
        TArray<FColor> Frame;
        Frame.Reserve(256 * Height);
        for (int32 Row = 0; Row < Height; ++Row)
        {
            for (int32 Column = 0; Column < 256; ++Column)
            {
                const uint8 Level = static_cast<uint8>(Column);
                Frame.Add(FColor(Level, Level, Level, 255));
            }
        }
        return Frame;
    }

    // The pre-fix payload: what the verb published for EVERY frame, cold or finished, with
    // identical values. That indistinguishability is the whole complaint.
    TSharedPtr<FJsonObject> PWThumbLegacyPayload()
    {
        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetBoolField(TEXT("success"), true);
        Result->SetStringField(TEXT("assetPath"), TEXT("/PinWrightTest/Synthetic"));
        Result->SetNumberField(TEXT("width"), 256);
        Result->SetNumberField(TEXT("height"), 64);
        Result->SetStringField(TEXT("format"), TEXT("png"));
        return Result;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGenerateThumbnailFrameEvidenceTest,
    "PinWright.asset.generate_thumbnail.FrameEvidenceSeparatesColdFromReal",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGenerateThumbnailFrameEvidenceTest::RunTest(const FString& Parameters)
{
    constexpr int32 Height = 64;
    const PinWrightThumbnail::FThumbnailReadinessReport Readiness;
    const PinWrightThumbnail::FColdFrameRetryReport NoRetry;

    // --- 1. The entirely empty frame. `blank` catches this one. ---
    const PinWrightRenderCapture::FCaptureImageStats BlackStats =
        PinWrightRenderCapture::CalculateCaptureImageStats(
            PWThumbFlatFrame(256, Height, FColor(0, 0, 0, 255)));
    TSharedPtr<FJsonObject> BlackResult = PWThumbLegacyPayload();
    PinWrightThumbnail::AddFrameEvidenceFields(BlackStats, NoRetry, 2, Readiness, BlackResult);

    TestTrue(TEXT("an all-black frame is degenerate"),
        PinWrightThumbnail::FrameIsDegenerate(BlackStats));
    TestTrue(TEXT("an all-black frame reports blank"), BlackResult->GetBoolField(TEXT("blank")));
    TestTrue(TEXT("an all-black frame carries a frameWarning"),
        BlackResult->HasField(TEXT("frameWarning")));

    // --- 2. The washed-out uniform frame. `blank` says FINE here — this is the half a blank
    //        test structurally cannot see, and the reason the tone verdict is a separate field. ---
    const PinWrightRenderCapture::FCaptureImageStats GreyStats =
        PinWrightRenderCapture::CalculateCaptureImageStats(
            PWThumbFlatFrame(256, Height, FColor(160, 160, 160, 255)));
    TSharedPtr<FJsonObject> GreyResult = PWThumbLegacyPayload();
    PinWrightThumbnail::AddFrameEvidenceFields(GreyStats, NoRetry, 1, Readiness, GreyResult);

    TestFalse(TEXT("a flat grey frame is NOT blank"), GreyResult->GetBoolField(TEXT("blank")));
    TestTrue(TEXT("a flat grey frame is still degenerate"),
        PinWrightThumbnail::FrameIsDegenerate(GreyStats));
    TestTrue(TEXT("a flat grey frame is flagged crushed or blownOut"),
        GreyResult->GetBoolField(TEXT("crushed")) || GreyResult->GetBoolField(TEXT("blownOut")));
    TestTrue(TEXT("a flat grey frame carries a frameWarning"),
        GreyResult->HasField(TEXT("frameWarning")));

    // --- 3. A frame that was really drawn. ---
    const PinWrightRenderCapture::FCaptureImageStats RampStats =
        PinWrightRenderCapture::CalculateCaptureImageStats(PWThumbRampFrame(Height));
    TSharedPtr<FJsonObject> RampResult = PWThumbLegacyPayload();
    PinWrightThumbnail::AddFrameEvidenceFields(RampStats, NoRetry, 1, Readiness, RampResult);

    TestFalse(TEXT("a drawn frame is not degenerate"),
        PinWrightThumbnail::FrameIsDegenerate(RampStats));
    TestFalse(TEXT("a drawn frame is not blank"), RampResult->GetBoolField(TEXT("blank")));
    TestFalse(TEXT("a drawn frame is not crushed"), RampResult->GetBoolField(TEXT("crushed")));
    TestFalse(TEXT("a drawn frame is not blownOut"), RampResult->GetBoolField(TEXT("blownOut")));
    TestFalse(TEXT("a drawn frame carries no frameWarning"),
        RampResult->HasField(TEXT("frameWarning")));

    // --- THE COUNTERFACTUAL. The cold and the finished payload must now differ. Before the fix
    //     both objects were exactly PWThumbLegacyPayload() and compared equal field for field,
    //     which is what let a caller read a warm-up artefact as a change in the subject. ---
    const TSharedPtr<FJsonObject>* BlackImageStats = nullptr;
    const TSharedPtr<FJsonObject>* RampImageStats = nullptr;
    const bool bBothPublished =
        BlackResult->TryGetObjectField(TEXT("imageStats"), BlackImageStats) &&
        RampResult->TryGetObjectField(TEXT("imageStats"), RampImageStats);
    TestTrue(TEXT("both frames publish an imageStats block"), bBothPublished);
    if (bBothPublished)
    {
        TestNotEqual(TEXT("cold and finished frames do not report the same toneLevelsUsed"),
            (*BlackImageStats)->GetNumberField(TEXT("toneLevelsUsed")),
            (*RampImageStats)->GetNumberField(TEXT("toneLevelsUsed")));
        TestNotEqual(TEXT("cold and finished frames do not report the same meanLuminance"),
            (*BlackImageStats)->GetNumberField(TEXT("meanLuminance")),
            (*RampImageStats)->GetNumberField(TEXT("meanLuminance")));
        TestTrue(TEXT("cold and finished frames do not report the same blank verdict"),
            BlackResult->GetBoolField(TEXT("blank")) != RampResult->GetBoolField(TEXT("blank")));
    }

    // --- 4. An unmeasured frame must not read as the most collapsed one. A default-constructed
    //        FCaptureImageStats has ToneLevelsUsed 0; publishing it would hand the caller a
    //        verdict nobody reached. ---
    const PinWrightRenderCapture::FCaptureImageStats Unmeasured;
    TestFalse(TEXT("a default-constructed stats block is not marked measured"),
        Unmeasured.bStatsMeasured);
    TestTrue(TEXT("unmeasured pixels count as degenerate"),
        PinWrightThumbnail::FrameIsDegenerate(Unmeasured));
    TSharedPtr<FJsonObject> UnmeasuredResult = PWThumbLegacyPayload();
    PinWrightThumbnail::AddFrameEvidenceFields(Unmeasured, NoRetry, 1, Readiness, UnmeasuredResult);
    TestFalse(TEXT("an unmeasured frame publishes no imageStats"),
        UnmeasuredResult->HasField(TEXT("imageStats")));
    TestFalse(TEXT("an unmeasured frame does not claim its stats were measured"),
        UnmeasuredResult->GetBoolField(TEXT("imageStatsMeasured")));
    TestTrue(TEXT("an unmeasured frame still says so in a warning"),
        UnmeasuredResult->HasField(TEXT("frameWarning")));

    // --- 5. The cold-frame retry block. Two renders of identical inputs that disagree are the
    //        evidence that the first frame was still settling. ---
    PinWrightThumbnail::FColdFrameRetryReport Retry;
    Retry.bRetried = true;
    Retry.FirstPassStats = BlackStats;
    Retry.ComparedPixels = 256 * Height;
    Retry.DifferingPixels = PinWrightThumbnail::CountDifferingPixels(
        PWThumbFlatFrame(256, Height, FColor(0, 0, 0, 255)), PWThumbRampFrame(Height));
    // 16384 pixels, of which only the ramp's level-0 column matches black: 16320 differ.
    TestTrue(TEXT("a black frame and a ramp frame differ over almost every pixel"),
        Retry.DifferingPixels > (250 * Height));

    TSharedPtr<FJsonObject> RetriedResult = PWThumbLegacyPayload();
    PinWrightThumbnail::AddFrameEvidenceFields(RampStats, Retry, 2, Readiness, RetriedResult);
    const TSharedPtr<FJsonObject>* RetryObject = nullptr;
    if (TestTrue(TEXT("a retried render publishes coldFrameRetry"),
            RetriedResult->TryGetObjectField(TEXT("coldFrameRetry"), RetryObject)))
    {
        TestTrue(TEXT("the retry block reports the first pass was blank"),
            (*RetryObject)->GetBoolField(TEXT("firstPassBlank")));
        TestTrue(TEXT("the retry block reports the frame changed between passes"),
            (*RetryObject)->GetBoolField(TEXT("frameChanged")));
        TestTrue(TEXT("the retry block reports a differing fraction near one"),
            (*RetryObject)->GetNumberField(TEXT("differingFraction")) > 0.9);
        TestEqual(TEXT("a retried render reports two passes"),
            RetriedResult->GetNumberField(TEXT("renderPasses")), 2.0);
    }
    // A frame that never needed a second pass must not carry the block at all: an empty retry
    // report would read as "two renders agreed", which is a measurement nobody made.
    TestFalse(TEXT("an un-retried render publishes no coldFrameRetry"),
        RampResult->HasField(TEXT("coldFrameRetry")));

    // --- 6. Readiness is always reported, so an absent block cannot be read as "waited". ---
    TestTrue(TEXT("every response carries a readiness block"),
        RampResult->HasField(TEXT("readiness")));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGenerateThumbnailHandlerFrameEvidenceTest,
    "PinWright.asset.generate_thumbnail.HandlerPublishesMeasuredFrameEvidence",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGenerateThumbnailHandlerFrameEvidenceTest::RunTest(const FString& Parameters)
{
    // Synthetic engine content, not host content: the defect was found on a project material but
    // has nothing to do with one.
    const FString AssetPath = TEXT("/Engine/EngineMeshes/Cube");
    if (!LoadObject<UObject>(nullptr, *AssetPath))
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-asset-unavailable"),
            TEXT("/Engine/EngineMeshes/Cube did not load, so no thumbnail could be rendered."));
        return true;
    }

    const FString OutPath = FPaths::Combine(FPaths::ProjectSavedDir(),
        TEXT("PinWright"), TEXT("Tests"), TEXT("GenerateThumbnailFrameEvidence.png"));
    IFileManager::Get().Delete(*OutPath, false, true, true);

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), AssetPath);
    Payload->SetNumberField(TEXT("width"), 64);
    Payload->SetNumberField(TEXT("height"), 64);
    Payload->SetStringField(TEXT("outputPath"), OutPath);

    FTestResponseCapture Capture;
    TestTrue(TEXT("asset.generate_thumbnail handler found"),
        InvokeHandlerWithCapture(TEXT("asset.generate_thumbnail"), Payload, Capture));

    if (!Capture.bSuccess || !Capture.Result.IsValid())
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-rhi-thumbnail-render"),
            FString::Printf(TEXT("Thumbnail rendering is unavailable on this host (errorCode=%s), ")
                            TEXT("so the published statistics could not be checked against pixels."),
                *Capture.ErrorCode));
        return true;
    }
    ON_SCOPE_EXIT { IFileManager::Get().Delete(*OutPath, false, true, true); };

    // The block exists at all. Its absence is the shipped defect.
    const TSharedPtr<FJsonObject>* ImageStats = nullptr;
    if (!TestTrue(TEXT("the response publishes imageStats"),
            Capture.Result->TryGetObjectField(TEXT("imageStats"), ImageStats)))
    {
        return true;
    }
    TestTrue(TEXT("imageStats carries the tone-range count"),
        (*ImageStats)->HasField(TEXT("toneLevelsUsed")));
    TestTrue(TEXT("the response carries a blank verdict"), Capture.Result->HasField(TEXT("blank")));
    TestTrue(TEXT("the response reports how many render passes it took"),
        Capture.Result->HasField(TEXT("renderPasses")));
    TestTrue(TEXT("the response reports what it waited for"),
        Capture.Result->HasField(TEXT("readiness")));

    const double RenderPasses = Capture.Result->GetNumberField(TEXT("renderPasses"));
    TestTrue(TEXT("renderPasses is one or two, never an unbounded warm-up"),
        RenderPasses >= 1.0 && RenderPasses <= 2.0);
    TestTrue(TEXT("coldFrameRetry is present exactly when a second pass ran"),
        Capture.Result->HasField(TEXT("coldFrameRetry")) == (RenderPasses > 1.0));

    // THE PIXEL ASSERTION. Re-measure the file that was actually written and require the published
    // numbers to describe it. This is the check the write path cannot fake: the statistics come
    // from the render buffer, the comparison comes from the decoded PNG, and a response that
    // published numbers unrelated to its own bytes fails here (rpc-design.md section 4).
    FImage Written;
    if (!FImageUtils::LoadImage(*OutPath, Written))
    {
        AddError(TEXT("the written thumbnail does not decode"));
        return true;
    }
    Written.ChangeFormat(ERawImageFormat::BGRA8, EGammaSpace::sRGB);
    TArrayView64<FColor> WrittenPixels = Written.AsBGRA8();
    TestTrue(TEXT("the written thumbnail has pixels"), WrittenPixels.Num() > 0);

    const TArray<FColor> Recomputed(WrittenPixels.GetData(), static_cast<int32>(WrittenPixels.Num()));
    const PinWrightRenderCapture::FCaptureImageStats FileStats =
        PinWrightRenderCapture::CalculateCaptureImageStats(Recomputed);

    TestEqual(TEXT("the published meanLuminance describes the bytes on disk"),
        (*ImageStats)->GetNumberField(TEXT("meanLuminance")), FileStats.MeanLuminance,
        UE_DOUBLE_KINDA_SMALL_NUMBER);
    TestEqual(TEXT("the published maxLuminance describes the bytes on disk"),
        (*ImageStats)->GetNumberField(TEXT("maxLuminance")), FileStats.MaxLuminance,
        UE_DOUBLE_KINDA_SMALL_NUMBER);
    TestEqual(TEXT("the published toneLevelsUsed describes the bytes on disk"),
        static_cast<int32>((*ImageStats)->GetNumberField(TEXT("toneLevelsUsed"))),
        FileStats.ToneLevelsUsed);
    TestTrue(TEXT("the published blank verdict describes the bytes on disk"),
        Capture.Result->GetBoolField(TEXT("blank")) == FileStats.bBlank);

    return true;
}

// ============================================================================
// B-thumbnail-primitive-ignored-on-instances
// ============================================================================
// The engine resolves the force-plane gate against the material INSTANCE's base material
// (FMaterialThumbnailScene::SetMaterialInterface -> GetBaseMaterial()->ShouldForcePlanePreview()),
// while the preview override wrote bUserModifiedShape only onto the asset it was handed. On an
// instance of a Niagara/particle-flagged master the flag therefore landed on the wrong object,
// the rule stayed armed, and the requested primitive was replaced by a flat plane - while the
// master rendered the shape correctly and the response echoed the request either way.
//
// Counterfactual: delete the ApplyBaseMaterialShapeFlag call in FScopedPreviewOverride and the
// instance case below reports WasForcedToPlane() == true while the master reports false, which
// is the ticket's exact "same request, one asset apart" divergence.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGenerateThumbnailInstanceShapeTest,
    "PinWright.asset.generate_thumbnail.InstanceResolvesSameShapeAsMaster",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGenerateThumbnailInstanceShapeTest::RunTest(const FString& Parameters)
{
    FString MasterPath;
    FString InstancePath;
    UMaterial* Master = nullptr;
    UMaterialInstanceConstant* Instance = nullptr;
    if (!PWThumbMakeMasterAndInstance(*this, TEXT("Shape"), MasterPath, InstancePath, Master, Instance))
    {
        return true;
    }
    ON_SCOPE_EXIT
    {
        CleanupTestAsset(InstancePath);
        CleanupTestAsset(MasterPath);
    };

    if (!PWThumbArmNiagaraMeshUsage(Master))
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("niagara-usage-flag-not-reflected"),
            TEXT("UMaterial.bUsedWithNiagaraMeshParticles was not found by reflection, so the "
                 "force-plane rule this test exists to defeat could not be armed"));
        return true;
    }

    // Non-vacuity, both halves. If the rule were not actually armed, every "not forced" reading
    // below would pass on an inert fixture and prove nothing.
    FString Reason;
    TestTrue(TEXT("the master is force-planed before any override"),
        PinWrightThumbnail::WillForcePlanePreview(Master, Reason));
    TestTrue(TEXT("the instance is force-planed before any override"),
        PinWrightThumbnail::WillForcePlanePreview(Instance, Reason));

    PinWrightThumbnail::FPreviewOverrideRequest Request;
    bool bCustomMesh = false;
    TestTrue(TEXT("'sphere' parses"),
        PinWrightThumbnail::ParsePrimitiveName(TEXT("sphere"), Request.PrimitiveType, bCustomMesh));
    Request.bHasPrimitive = true;

    bool bMasterForced = true;
    {
        FString Code;
        FString Message;
        PinWrightThumbnail::FScopedPreviewOverride Guard(Master, Request, Code, Message);
        TestTrue(FString::Printf(TEXT("the master override applies (%s)"), *Message), Guard.IsValid());
        bMasterForced = Guard.WasForcedToPlane();
    }

    bool bInstanceForced = true;
    {
        FString Code;
        FString Message;
        PinWrightThumbnail::FScopedPreviewOverride Guard(Instance, Request, Code, Message);
        TestTrue(FString::Printf(TEXT("the instance override applies (%s)"), *Message), Guard.IsValid());
        bInstanceForced = Guard.WasForcedToPlane();
    }

    // Asserted apart so a failure names which half broke, then compared, which is the ticket's
    // own claim: the same request one asset apart must not resolve to two different shapes.
    TestFalse(TEXT("a requested primitive is honoured on the master"), bMasterForced);
    TestFalse(TEXT("a requested primitive is honoured on its instance"), bInstanceForced);
    TestTrue(TEXT("master and instance resolve to the same preview shape"),
        bInstanceForced == bMasterForced);

    // The master is a SECOND asset the guard borrowed. It must be handed back exactly as found,
    // or one thumbnail call would leave every future render of that material shape-pinned.
    TestTrue(TEXT("the master's force-plane rule is armed again after the scope"),
        PinWrightThumbnail::WillForcePlanePreview(Instance, Reason));

    return true;
}

// The other half of the same ticket: when the engine genuinely forces a plane and the guard
// cannot defeat it, the response must report the shape DRAWN rather than echo the request.
//
// Counterfactual: restore the old SetStringField("primitive", PrimitiveName) echo and this reads
// "sphere" for a frame that is a flat quad, with no requestedPrimitive to contradict it.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGenerateThumbnailForcedPlaneReportedTest,
    "PinWright.asset.generate_thumbnail.ForcedPlaneIsReportedNotEchoed",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGenerateThumbnailForcedPlaneReportedTest::RunTest(const FString& Parameters)
{
    FString MasterPath;
    FString InstancePath;
    UMaterial* Master = nullptr;
    UMaterialInstanceConstant* Instance = nullptr;
    if (!PWThumbMakeMasterAndInstance(*this, TEXT("Forced"), MasterPath, InstancePath, Master, Instance))
    {
        return true;
    }
    ON_SCOPE_EXIT
    {
        CleanupTestAsset(InstancePath);
        CleanupTestAsset(MasterPath);
    };

    // An unconditional force: unlike the particle/Niagara usage flags this one is not gated on
    // bUserModifiedShape, so no override can rescue it and the only honest move left is to say
    // what was drawn.
    Master->SetShouldForcePlanePreview(true);

    FString Reason;
    TestTrue(TEXT("the fixture really is force-planed"),
        PinWrightThumbnail::WillForcePlanePreview(Instance, Reason));
    TestTrue(TEXT("the force-plane predicate names a reason"), !Reason.IsEmpty());

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), InstancePath);
    Payload->SetStringField(TEXT("primitive"), TEXT("sphere"));
    Payload->SetNumberField(TEXT("width"), 64);
    Payload->SetNumberField(TEXT("height"), 64);

    FTestResponseCapture Capture;
    TestTrue(TEXT("asset.generate_thumbnail handler found"),
        InvokeHandlerWithCapture(TEXT("asset.generate_thumbnail"), Payload, Capture));

    if (!Capture.bSuccess || !Capture.Result.IsValid())
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("thumbnail-render-unavailable"),
            FString::Printf(TEXT("asset.generate_thumbnail did not render the synthetic fixture "
                                 "(errorCode=%s); the response-shape assertions were not run"),
                *Capture.ErrorCode));
        return true;
    }

    FString DrawnPrimitive;
    Capture.Result->TryGetStringField(TEXT("primitive"), DrawnPrimitive);
    TestEqual(TEXT("the response reports the shape actually drawn"),
        DrawnPrimitive, FString(TEXT("plane")));

    FString RequestedPrimitive;
    Capture.Result->TryGetStringField(TEXT("requestedPrimitive"), RequestedPrimitive);
    TestEqual(TEXT("the response keeps the request beside it"),
        RequestedPrimitive, FString(TEXT("sphere")));

    FString ReportedReason;
    Capture.Result->TryGetStringField(TEXT("primitiveReason"), ReportedReason);
    TestTrue(TEXT("the substitution carries a reason"), !ReportedReason.IsEmpty());

    return true;
}

// ============================================================================
// B-thumbnail-plane-elevation-edge-on
// ============================================================================
// The thumbnail plane is a zero-thickness quad under a constant rotation, so an orbit away from
// its one fixed attitude renders it edge on: primitive:"plane" with elevation:89 produced a ~2px
// sliver in an otherwise empty frame while the response was indistinguishable from a good one.
// The engine guards its own forced-plane path against exactly this and leaves the
// caller-requested plane path unguarded.
//
// Counterfactual: drop the "&& !bResolvedShapeIsPlane" conditions in FScopedPreviewOverride and
// the elevated frame's coverage collapses to a couple of percent while the control keeps its
// framing - which is the difference this test measures.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGenerateThumbnailPlaneFramingTest,
    "PinWright.asset.generate_thumbnail.PlaneKeepsFramingUnderElevation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGenerateThumbnailPlaneFramingTest::RunTest(const FString& Parameters)
{
    const FString AssetPath = TEXT("/Engine/EngineMaterials/DefaultMaterial");
    if (!UEditorAssetLibrary::DoesAssetExist(AssetPath))
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("engine-default-material-missing"),
            TEXT("/Engine/EngineMaterials/DefaultMaterial is unavailable in this run"));
        return true;
    }

    const FString ControlPath = FPaths::Combine(FPaths::ProjectSavedDir(),
        TEXT("PinWright"), TEXT("Tests"), TEXT("ThumbnailPlaneControl.png"));
    const FString ElevatedPath = FPaths::Combine(FPaths::ProjectSavedDir(),
        TEXT("PinWright"), TEXT("Tests"), TEXT("ThumbnailPlaneElevated.png"));
    IFileManager::Get().Delete(*ControlPath, false, true, true);
    IFileManager::Get().Delete(*ElevatedPath, false, true, true);
    ON_SCOPE_EXIT
    {
        IFileManager::Get().Delete(*ControlPath, false, true, true);
        IFileManager::Get().Delete(*ElevatedPath, false, true, true);
    };

    // Control: the plane at whatever angle the asset itself stores. This is the frame the ticket
    // reports as correct, and it is what makes the coverage metric below mean something.
    TSharedPtr<FJsonObject> ControlPayload = MakeShared<FJsonObject>();
    ControlPayload->SetStringField(TEXT("assetPath"), AssetPath);
    ControlPayload->SetStringField(TEXT("primitive"), TEXT("plane"));
    ControlPayload->SetNumberField(TEXT("width"), 128);
    ControlPayload->SetNumberField(TEXT("height"), 128);
    ControlPayload->SetStringField(TEXT("outputPath"), ControlPath);

    FTestResponseCapture ControlCapture;
    TestTrue(TEXT("asset.generate_thumbnail handler found"),
        InvokeHandlerWithCapture(TEXT("asset.generate_thumbnail"), ControlPayload, ControlCapture));

    // Probe: the same request plus an elevation that would put the camera almost overhead.
    TSharedPtr<FJsonObject> ElevatedPayload = MakeShared<FJsonObject>();
    ElevatedPayload->SetStringField(TEXT("assetPath"), AssetPath);
    ElevatedPayload->SetStringField(TEXT("primitive"), TEXT("plane"));
    ElevatedPayload->SetNumberField(TEXT("width"), 128);
    ElevatedPayload->SetNumberField(TEXT("height"), 128);
    ElevatedPayload->SetNumberField(TEXT("elevation"), 89);
    ElevatedPayload->SetStringField(TEXT("outputPath"), ElevatedPath);

    FTestResponseCapture ElevatedCapture;
    InvokeHandlerWithCapture(TEXT("asset.generate_thumbnail"), ElevatedPayload, ElevatedCapture);

    if (!ControlCapture.bSuccess || !ElevatedCapture.bSuccess)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("thumbnail-render-unavailable"),
            FString::Printf(TEXT("plane renders did not complete (control=%s elevated=%s); "
                                 "neither the response nor the pixel assertions were run"),
                *ControlCapture.ErrorCode, *ElevatedCapture.ErrorCode));
        return true;
    }

    // The clamp reports itself. Without this the two responses are byte-identical and a caller
    // cannot tell a framed plane from an empty frame.
    if (ElevatedCapture.Result.IsValid())
    {
        bool bElevationApplied = true;
        TestTrue(TEXT("the dropped elevation is reported"),
            ElevatedCapture.Result->TryGetBoolField(TEXT("elevationApplied"), bElevationApplied));
        TestFalse(TEXT("elevationApplied says false"), bElevationApplied);

        FString CameraReason;
        ElevatedCapture.Result->TryGetStringField(TEXT("cameraReason"), CameraReason);
        TestTrue(TEXT("the drop carries a reason"), !CameraReason.IsEmpty());
    }
    if (ControlCapture.Result.IsValid())
    {
        // ...and only when something was actually dropped, so the field stays a signal.
        TestFalse(TEXT("a request with no angle carries no cameraReason"),
            ControlCapture.Result->HasField(TEXT("cameraReason")));
    }

    const double ControlCoverage = PWThumbMeasureCoverage(ControlPath);
    const double ElevatedCoverage = PWThumbMeasureCoverage(ElevatedPath);

    // The control decides whether the metric can discriminate on this host at all: it must show
    // a plane that covers a real part of the frame against a background it can be told apart
    // from. Asserting the probe against a control that measured nothing would be theatre.
    if (ControlCoverage < 0.15 || ControlCoverage > 0.98)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("plane-coverage-not-measurable"),
            FString::Printf(TEXT("the control plane frame covers %.4f of the image, outside the "
                                 "(0.15, 0.98) band the metric can discriminate in"),
                ControlCoverage));
        return true;
    }

    TestTrue(FString::Printf(
                 TEXT("an elevated plane still covers a plausible part of the frame "
                      "(%.4f, control %.4f)"),
                 ElevatedCoverage, ControlCoverage),
        ElevatedCoverage > 0.15);
    TestTrue(FString::Printf(
                 TEXT("the elevated frame matches the control framing (%.4f vs %.4f)"),
                 ElevatedCoverage, ControlCoverage),
        FMath::Abs(ElevatedCoverage - ControlCoverage) < 0.05);

    return true;
}
