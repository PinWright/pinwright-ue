// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression tests for board B-combine-textures-leaks-bulkdata-lock-then-crashes and
// B-texture-action-bulkdata-lock-assert-fatal.
//
// texture.combine_textures used to read its two inputs from their *built platform* mip
// (GetPlatformData()->Mips[0].BulkData.LockReadOnly()). FBulkData::LockReadOnly
// (BulkData.cpp) sets the lock status and only THEN returns GetDataBufferReadOnly(), which
// is null for a compressed / GPU-resident payload — so a null return still means LOCKED.
// The handler's guard was `if (BaseData) BaseMip.BulkData.Unlock();`, i.e. it skipped the
// unlock in exactly the case where the lock had been taken. The payload stayed locked, the
// caller got a soft "Failed to lock texture data" that invited a retry, and the retry hit
// `check(IsUnlocked())` inside LockReadOnly — a Fatal assertion that ended the editor
// process and every co-tenant agent's unsaved work with it.
//
// The fix routes every texture lock through TextureSourceMip::FScopedMipLock, which locks
// the editor FTextureSource (CPU-resident, uncompressed) via the engine's own
// FTextureSource::FMipLock RAII pair. Two properties are asserted here:
//
//  1. ReadsEditorSource — the blend reads the editable source, not the platform mip. Both
//     inputs carry a distinctive constant colour in Source while their platform mips are
//     left resident-but-FLAT (all zeros). A revert to the platform read produces a black
//     output and fails the colour assertion. Keeping the platform mip resident rather than
//     absent is what makes a revert fail cleanly instead of crashing the suite.
//  2. FailedLockReleasesSourceLock — a combine that fails on its SECOND input (base locked,
//     overlay refused) leaves nothing locked. Proven by driving texture.invert in place on
//     the base afterwards: an in-place invert needs a ReadWrite lock on the same
//     FTextureSource, and FTextureSource::LockMipInternal refuses ReadWrite while a read
//     lock is held ("cannot lock for write when previously locked for read", returning
//     null). So a leaked read lock turns the follow-up invert into a failure — observable,
//     and without the fatal assertion the FBulkData path used to raise, which could not be
//     asserted on in-process at all.
#include "Misc/AutomationTest.h"

#include "Dom/JsonObject.h"
#include "Engine/Texture.h"
#include "Engine/Texture2D.h"
#include "PixelFormat.h"
#include "TextureResource.h"
#include "Tests/TestUtils.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
    // Builds a named /Game UTexture2D whose editor Source is a constant BGRA8 colour and
    // whose platform mip is resident but all zeros. The named package (not the transient
    // one) is required because the handler resolves its inputs with StaticLoadObject.
    UTexture2D* MakeConstantSourceTexture(const FString& PkgPath, int32 N,
        uint8 B, uint8 G, uint8 R, uint8 A, bool bWithSource = true)
    {
        UPackage* Package = CreatePackage(*PkgPath);
        if (!Package)
        {
            return nullptr;
        }
        const FString AssetName = FPackageName::GetLongPackageAssetName(PkgPath);
        UTexture2D* Texture = NewObject<UTexture2D>(Package, *AssetName, RF_Public | RF_Standalone);
        if (!Texture)
        {
            return nullptr;
        }

        // Resident-but-flat platform mip: only the (reverted) platform-read path would see
        // it, and seeing zeros there fails an assertion instead of dereferencing null.
        Texture->SetPlatformData(new FTexturePlatformData());
        Texture->GetPlatformData()->SizeX = N;
        Texture->GetPlatformData()->SizeY = N;
        Texture->GetPlatformData()->PixelFormat = PF_B8G8R8A8;
        FTexture2DMipMap* Mip = new FTexture2DMipMap();
        Mip->SizeX = N;
        Mip->SizeY = N;
        Texture->GetPlatformData()->Mips.Add(Mip);
        {
            const int64 FlatBytes = static_cast<int64>(N) * N * 4;
            Mip->BulkData.Lock(LOCK_READ_WRITE);
            void* Dst = Mip->BulkData.Realloc(FlatBytes);
            FMemory::Memzero(Dst, FlatBytes);
            Mip->BulkData.Unlock();
        }

        if (bWithSource)
        {
            TArray<uint8> Bytes;
            Bytes.SetNumUninitialized(N * N * 4);
            for (int32 i = 0; i < N * N; ++i)
            {
                Bytes[i * 4 + 0] = B;
                Bytes[i * 4 + 1] = G;
                Bytes[i * 4 + 2] = R;
                Bytes[i * 4 + 3] = A;
            }
            Texture->Source.Init(N, N, 1, 1, TSF_BGRA8, Bytes.GetData());
        }

        return Texture;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCombineTexturesReadsEditorSourceTest,
    "PinWright.texture.combine_textures.ReadsEditorSource",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCombineTexturesReadsEditorSourceTest::RunTest(const FString& Parameters)
{
    const int32 N = 16;
    const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString BasePkg = FString::Printf(TEXT("/Game/PinWrightTests/T_CmbBase_%s"), *Suffix);
    const FString OverlayPkg = FString::Printf(TEXT("/Game/PinWrightTests/T_CmbOvl_%s"), *Suffix);
    const FString OutFolder = TEXT("/Game/PinWrightTests");
    const FString OutName = FString::Printf(TEXT("T_CmbOut_%s"), *Suffix);
    const FString OutPkg = FString::Printf(TEXT("%s/%s"), *OutFolder, *OutName);

    // Base is mid-grey (128), overlay is white (255). Multiply at opacity 1 keeps the base
    // value; the platform mips are zero, so a revert produces 0 instead.
    UTexture2D* BaseTex = MakeConstantSourceTexture(BasePkg, N, 128, 128, 128, 255);
    UTexture2D* OverlayTex = MakeConstantSourceTexture(OverlayPkg, N, 255, 255, 255, 255);
    if (!TestNotNull(TEXT("base texture created"), BaseTex) ||
        !TestNotNull(TEXT("overlay texture created"), OverlayTex))
    {
        return true;
    }
    ON_SCOPE_EXIT
    {
        // save:false keeps everything in memory; clear dirty so the host is not left
        // carrying unsaved packages.
        if (BaseTex) { BaseTex->GetOutermost()->SetDirtyFlag(false); }
        if (OverlayTex) { OverlayTex->GetOutermost()->SetDirtyFlag(false); }
        if (UObject* Out = StaticFindObject(UTexture2D::StaticClass(), nullptr, *ToObjectPath(OutPkg)))
        {
            Out->GetOutermost()->SetDirtyFlag(false);
        }
    };

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("baseTexture"), ToObjectPath(BasePkg));
    Payload->SetStringField(TEXT("overlayTexture"), ToObjectPath(OverlayPkg));
    Payload->SetStringField(TEXT("blendMode"), TEXT("Multiply"));
    Payload->SetNumberField(TEXT("opacity"), 1.0);
    Payload->SetStringField(TEXT("name"), OutName);
    Payload->SetStringField(TEXT("path"), OutFolder);
    Payload->SetBoolField(TEXT("save"), false);

    FTestResponseCapture Capture;
    if (!TestTrue(TEXT("combine_textures handler registered"),
            InvokeHandlerWithCapture(TEXT("texture.combine_textures"), Payload, Capture)))
    {
        return true;
    }
    TestTrue(*FString::Printf(TEXT("combine_textures succeeds (err='%s': %s)"),
                 *Capture.ErrorCode, *Capture.Message),
        Capture.bSuccess);
    if (!Capture.bSuccess)
    {
        return true;
    }

    UTexture2D* OutTex = Cast<UTexture2D>(
        StaticFindObject(UTexture2D::StaticClass(), nullptr, *ToObjectPath(OutPkg)));
    if (!TestNotNull(TEXT("output texture exists"), OutTex))
    {
        return true;
    }
    if (!TestTrue(TEXT("output has editor source data"), OutTex->Source.IsValid()))
    {
        return true;
    }

    const uint8* OutPixels = OutTex->Source.LockMipReadOnly(0);
    if (!TestNotNull(TEXT("output source mip readable"), OutPixels))
    {
        return true;
    }
    const uint8 FirstB = OutPixels[0];
    const uint8 FirstG = OutPixels[1];
    const uint8 FirstR = OutPixels[2];
    OutTex->Source.UnlockMip(0);

    // 128 * 255/255 == 128, allowing one unit of rounding slack. A revert to the flat
    // platform mip yields 0 on every channel.
    TestTrue(*FString::Printf(
                 TEXT("blend read the editor source, not the flat platform mip (B=%d G=%d R=%d, expected ~128)"),
                 static_cast<int32>(FirstB), static_cast<int32>(FirstG), static_cast<int32>(FirstR)),
        FirstB >= 127 && FirstB <= 129 && FirstG >= 127 && FirstG <= 129 && FirstR >= 127 && FirstR <= 129);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCombineTexturesFailedLockReleasesSourceLockTest,
    "PinWright.texture.combine_textures.FailedLockReleasesSourceLock",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCombineTexturesFailedLockReleasesSourceLockTest::RunTest(const FString& Parameters)
{
    const int32 N = 16;
    const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString BasePkg = FString::Printf(TEXT("/Game/PinWrightTests/T_LeakBase_%s"), *Suffix);
    const FString OverlayPkg = FString::Printf(TEXT("/Game/PinWrightTests/T_LeakOvl_%s"), *Suffix);
    const FString OutFolder = TEXT("/Game/PinWrightTests");
    const FString OutName = FString::Printf(TEXT("T_LeakOut_%s"), *Suffix);

    UTexture2D* BaseTex = MakeConstantSourceTexture(BasePkg, N, 40, 80, 120, 255);
    // The overlay deliberately has NO editor source: the handler locks the base first, then
    // refuses on the overlay, so the base's lock must survive nothing but its own guard.
    UTexture2D* OverlayTex = MakeConstantSourceTexture(OverlayPkg, N, 0, 0, 0, 255, /*bWithSource=*/false);
    if (!TestNotNull(TEXT("base texture created"), BaseTex) ||
        !TestNotNull(TEXT("sourceless overlay texture created"), OverlayTex))
    {
        return true;
    }
    const FString OutPkg = FString::Printf(TEXT("%s/%s"), *OutFolder, *OutName);
    ON_SCOPE_EXIT
    {
        if (BaseTex) { BaseTex->GetOutermost()->SetDirtyFlag(false); }
        if (OverlayTex) { OverlayTex->GetOutermost()->SetDirtyFlag(false); }
        // The output package is created before the overlay lock is refused, so it exists even
        // though the call failed.
        if (UObject* Out = StaticFindObject(UTexture2D::StaticClass(), nullptr, *ToObjectPath(OutPkg)))
        {
            Out->GetOutermost()->SetDirtyFlag(false);
        }
    };

    TSharedPtr<FJsonObject> CombinePayload = MakeShared<FJsonObject>();
    CombinePayload->SetStringField(TEXT("baseTexture"), ToObjectPath(BasePkg));
    CombinePayload->SetStringField(TEXT("overlayTexture"), ToObjectPath(OverlayPkg));
    CombinePayload->SetStringField(TEXT("name"), OutName);
    CombinePayload->SetStringField(TEXT("path"), OutFolder);
    CombinePayload->SetBoolField(TEXT("save"), false);

    FTestResponseCapture CombineCapture;
    if (!TestTrue(TEXT("combine_textures handler registered"),
            InvokeHandlerWithCapture(TEXT("texture.combine_textures"), CombinePayload, CombineCapture)))
    {
        return true;
    }
    TestFalse(TEXT("combine_textures refuses the sourceless overlay"), CombineCapture.bSuccess);
    // The old message named neither the asset nor which side failed; the ticket asked for both.
    TestTrue(*FString::Printf(TEXT("refusal names the overlay side (message: %s)"), *CombineCapture.Message),
        CombineCapture.Message.Contains(TEXT("overlay")));

    // The leak detector: an in-place invert needs a ReadWrite lock on the base's own
    // FTextureSource. A read lock left behind by the failed combine makes that lock refused
    // (LockMipInternal returns null), so this call fails.
    TSharedPtr<FJsonObject> InvertPayload = MakeShared<FJsonObject>();
    InvertPayload->SetStringField(TEXT("assetPath"), ToObjectPath(BasePkg));
    InvertPayload->SetBoolField(TEXT("inPlace"), true);
    InvertPayload->SetBoolField(TEXT("save"), false);

    FTestResponseCapture InvertCapture;
    if (!TestTrue(TEXT("invert handler registered"),
            InvokeHandlerWithCapture(TEXT("texture.invert"), InvertPayload, InvertCapture)))
    {
        return true;
    }
    TestTrue(*FString::Printf(
                 TEXT("base source is unlocked after the failed combine (invert err='%s': %s)"),
                 *InvertCapture.ErrorCode, *InvertCapture.Message),
        InvertCapture.bSuccess);

    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
