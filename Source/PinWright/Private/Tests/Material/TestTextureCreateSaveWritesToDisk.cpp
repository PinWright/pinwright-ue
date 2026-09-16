// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for B-texture-save-no-disk-write.
//
// The entire texture.* write surface routes its save:true path through the shared
// mark-dirty no-op McpSafeAssetSave (Utils/AssetUtils.cpp: MarkPackageDirty +
// FAssetRegistryModule::AssetCreated then return true, no package-save API), so a
// texture create/mutate/set with save:true (the documented default) reports an
// unqualified success while nothing is ever written to disk — the edit lives only
// in memory + the registry and vanishes on a cold editor restart / git reset.
// texture.create_noise_texture (TextureHandler.cpp: `if (bSave) { AssetCreated;
// McpSafeAssetSave(NewTexture); }`) is a create-from-scratch instance of that family:
// its response carries existsAfter:true (via AddAssetVerification) but no honest
// saved / pendingFlush field, and no .uasset lands on disk.
//
// This test drives the real registered texture.create_noise_texture handler through
// the dispatcher via the shared TestCreateHandlerSaveWritesToDisk contract runner —
// the same runner the accepted material / audio / niagara create-save fixes adopted —
// and asserts the honest persistence contract: with save:true the new .uasset must be
// FileSize-probed on disk and the response must report saved:true. On current
// (pre-fix) source both fail: no file is written (OnDiskSize < 0) and the response
// has no saved field, so the runner's "save:true reports saved:true" and "the .uasset
// is on disk after save:true" assertions go red. The accepted fix (route the texture
// save:true path through SaveAssetToDiskReportingPresence + the ShouldTreatAssetSaveAsSuccess
// disk gate + AddAssetSaveReport) flips both green.

#include "Misc/AutomationTest.h"
#include "Misc/App.h"
#include "PixelFormat.h"
#include "Engine/Texture.h"
#include "Engine/Texture2D.h"
#include "TextureCompiler.h"
#include "TextureResource.h"

#include "Tests/TestSkipReporting.h"
#include "Tests/TestUtils.h"

// save:true must persist the new noise texture to disk and report saved:true.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTextureCreateNoiseSaveWritesToDiskTest,
    "PinWright.texture.create_noise_texture.SaveWritesToDisk",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FTextureCreateNoiseSaveWritesToDiskTest::RunTest(const FString& Parameters)
{
    return TestCreateHandlerSaveWritesToDisk(*this,
        TEXT("texture.create_noise_texture"), UTexture2D::StaticClass(),
        TEXT("T_NoiseSave"), TEXT("/Game/PinWrightTests/Texture"),
        /*bSave=*/true, /*bExpectDiskFile=*/true);
}

// Regression test for B-create-empty-texture-mip-size-wrong-for-float-rgba.
//
// CreateEmptyTexture (TextureHandler.cpp) used to hand-build a platform mip beside the editor
// Source buffer and size it `bHDR ? 16 : 4` bytes per pixel. PF_FloatRGBA is 8
// (Core/Private/Misc/PixelFormat.cpp: BlockBytes=8), so the HDR buffer was double the size the
// format it was tagged with prescribes. Nothing in the file ever read that buffer - all eleven
// call sites write through Source.LockMip(0), and every GetPlatformData()->Mips[0] read in the
// file is on a texture loaded from disk - so the fix deletes the hand-built platform data
// outright rather than correcting the constant.
//
// WHAT THIS TEST CAN AND CANNOT PROVE, stated plainly. On the default DDC1 host the defect is
// unobservable through the public surface: UpdateResource() -> UTexture::CachePlatformData()
// compares the existing platform data's derived-data key against the one it wants, a hand-built
// FTexturePlatformData carries the default (empty FString) key, and an empty key never matches,
// so the engine rebuilds and discards the wrong-sized mip before anything can read it. These
// assertions therefore pass on pre-fix source on such a host. They are not decorative: the same
// comparison under DDC2 (`[TextureBuild] NewTextureBuilds`, or -DDC2TextureBuilds) reads the key
// out of the variant's OTHER alternative, gets nullptr from a default-constructed variant, leaves
// bPerformCache false and ships the hand-built mips - and there both assertions below go red on
// pre-fix source. They are the closest meaningful assertions available, and they hold on every
// host after the fix because there is no longer any platform data for the engine to keep.
namespace TextureCreatePlatformMipHelpers
{
    // Named rather than anonymous: this file is Unity-merged with its neighbours, where two
    // anonymous namespaces exposing the same symbol collide. Every name here is distinct from
    // the ones in TestNoiseTextureFormatAndOctaves.cpp / TestNoiseTextureAlgorithmAndTiling.cpp,
    // whose file-scope `using namespace` is visible in this translation unit after a merge.
    static const TCHAR* const PlatformMipTestFolder = TEXT("/Game/PinWrightTests/Texture");

    // Square, power-of-two and small: the mip chain the assertions read only needs the texture to
    // be a normal mip-generating texture, and every pixel of it is irrelevant here.
    static const int32 PlatformMipTestSize = 32;

    FString MakePlatformMipAssetName()
    {
        return FString::Printf(TEXT("T_NoisePlatformMip_%s"),
            *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    }

    UTexture2D* FindPlatformMipAsset(const FString& AssetName)
    {
        const FString PackagePath = FString::Printf(TEXT("%s/%s"), PlatformMipTestFolder, *AssetName);
        return Cast<UTexture2D>(
            StaticFindObject(UTexture2D::StaticClass(), nullptr, *ToObjectPath(PackagePath)));
    }

    // save:false wrote nothing to disk, so clearing the dirty flag is the whole teardown.
    void DeDirtyPlatformMipAsset(const FString& AssetName)
    {
        if (UTexture2D* Created = FindPlatformMipAsset(AssetName))
        {
            if (UPackage* Pkg = Created->GetOutermost())
            {
                Pkg->SetDirtyFlag(false);
            }
        }
    }
}

// The platform mips of a created HDR texture must be the engine's, built from Source - a full
// chain, with mip 0 holding exactly the bytes its recorded pixel format prescribes.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTextureCreatePlatformMipMatchesPixelFormatTest,
    "PinWright.texture.create_noise_texture.PlatformMipMatchesPixelFormat",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FTextureCreatePlatformMipMatchesPixelFormatTest::RunTest(const FString& Parameters)
{
    using namespace TextureCreatePlatformMipHelpers;

    const FString AssetName = MakePlatformMipAssetName();

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("name"), AssetName);
    Payload->SetStringField(TEXT("path"), PlatformMipTestFolder);
    Payload->SetStringField(TEXT("noiseType"), TEXT("Perlin"));
    Payload->SetNumberField(TEXT("width"), PlatformMipTestSize);
    Payload->SetNumberField(TEXT("height"), PlatformMipTestSize);
    Payload->SetNumberField(TEXT("scale"), 4.0);
    Payload->SetNumberField(TEXT("octaves"), 3);
    Payload->SetNumberField(TEXT("seed"), 9137);
    Payload->SetBoolField(TEXT("hdr"), true);
    Payload->SetBoolField(TEXT("save"), false);

    FTestResponseCapture Capture;
    const bool bFound = InvokeHandlerWithCapture(
        TEXT("texture.create_noise_texture"), Payload, Capture);
    TestTrue(TEXT("texture.create_noise_texture is registered"), bFound);
    TestTrue(TEXT("texture.create_noise_texture responded"), Capture.bWasCalled);
    if (!TestTrue(TEXT("texture.create_noise_texture succeeded"), Capture.bSuccess))
    {
        return false;
    }

    UTexture2D* Created = FindPlatformMipAsset(AssetName);
    if (!TestNotNull(TEXT("the generated HDR texture is findable"), Created))
    {
        return false;
    }

    // The platform build is async by default; the mips are not readable until it lands. This is
    // the same wait the normal-map path takes before touching a freshly created texture.
    FTextureCompilingManager::Get().FinishCompilation({ Created });

    const FTexturePlatformData* Platform = Created->GetPlatformData();
    // CachePlatformData only builds when FApp::CanEverRender(); a host without a renderer gets an
    // empty container and there is nothing here to measure. That is a host limitation, not a
    // defect, so it is reported as a skip rather than stepped over silently.
    if (!Platform || Platform->Mips.Num() == 0)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("texture-platform-data-not-built"),
            FString::Printf(TEXT("platformData=%s mips=%d canEverRender=%s"),
                Platform ? TEXT("present") : TEXT("null"),
                Platform ? Platform->Mips.Num() : 0,
                FApp::CanEverRender() ? TEXT("true") : TEXT("false")));
        DeDirtyPlatformMipAsset(AssetName);
        return true;
    }

    // 1. The chain is the engine's. The helper shipped MipGenSettings=TMGS_FromTextureGroup with
    //    LODGroup=TEXTUREGROUP_World on a 32x32 power-of-two texture, so a build from Source
    //    produces a full chain down to 1x1. The hand-built platform data contained exactly one
    //    mip and no build ran over it, so this counted 1 wherever the defect was live.
    TestTrue(*FString::Printf(
        TEXT("the platform mip chain is the engine's, not a single hand-built mip (mips=%d)"),
        Platform->Mips.Num()), Platform->Mips.Num() > 1);

    // 2. Mip 0 holds what its own recorded pixel format prescribes. This is the assertion the
    //    ticket is about: the hand-built mip was tagged PF_FloatRGBA (8 bytes per pixel) while
    //    holding Width*Height*16 bytes, so the buffer disagreed with the format beside it.
    const FTexture2DMipMap& TopMip = Platform->Mips[0];
    const int64 ResidentBytes = TopMip.BulkData.GetBulkDataSize();
    if (ResidentBytes <= 0)
    {
        // Mips built in the editor can be paged out to the DDC, leaving BulkData empty. Nothing
        // is wrong with the texture; there is simply no buffer here to measure.
        PinWrightTestSkip::SkipAssertions(*this, TEXT("texture-platform-mip-not-resident"),
            FString::Printf(TEXT("mip0 bulkBytes=%lld pagedToDerivedData=%s"),
                ResidentBytes, TopMip.IsPagedToDerivedData() ? TEXT("true") : TEXT("false")));
        DeDirtyPlatformMipAsset(AssetName);
        return true;
    }

    const EPixelFormat MipFormat = Platform->PixelFormat;
    const int64 PrescribedBytes = static_cast<int64>(
        GPixelFormats[MipFormat].Get2DImageSizeInBytes(TopMip.SizeX, TopMip.SizeY));
    TestEqual(*FString::Printf(
        TEXT("mip 0 holds the bytes %s prescribes for %dx%d"),
        GPixelFormats[MipFormat].Name, static_cast<int32>(TopMip.SizeX),
        static_cast<int32>(TopMip.SizeY)), ResidentBytes, PrescribedBytes);

    DeDirtyPlatformMipAsset(AssetName);
    return true;
}
