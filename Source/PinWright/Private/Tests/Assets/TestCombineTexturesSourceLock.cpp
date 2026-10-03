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
        // Only reached if the output was (wrongly) created before the overlay was refused.
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
    // Both input format checks run before the output is created, so a refused input leaves no
    // empty output asset behind (B-combine-textures-output-overwrites-input).
    TestNull(TEXT("a combine refused on its overlay leaves no output asset"),
        StaticFindObject(UTexture2D::StaticClass(), nullptr, *ToObjectPath(OutPkg)));

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

// Regression tests for board B-combine-textures-output-overwrites-input.
//
// Every texture verb that writes a new asset goes through CreateEmptyTexture (TextureHandler.cpp),
// which used to run NewObject on the output name with no existing-asset guard. NewObject over a
// live UTexture2D of the same name re-allocates it in place and Source.Init zero-fills it, so an
// output naming an input wiped that input before it was read, and any other existing texture was
// silently replaced. The helper now refuses an existing asset with ASSET_ALREADY_EXISTS before
// creating anything. Reverting the guard turns both refusals below into a wipe of the fixture's
// pixels (and, for create_noise_texture, a success).
namespace CombineTexturesOutputGuardTestHelpers
{
    // Reads the first BGRA pixel of a texture's editor source; false when unreadable.
    bool ReadFirstSourcePixel(UTexture2D* Texture, uint8 (&OutBgra)[4])
    {
        if (!Texture || !Texture->Source.IsValid())
        {
            return false;
        }
        const uint8* Pixels = Texture->Source.LockMipReadOnly(0);
        if (!Pixels)
        {
            return false;
        }
        FMemory::Memcpy(OutBgra, Pixels, 4);
        Texture->Source.UnlockMip(0);
        return true;
    }

    // A /Game texture with an editable G8 source: it has data (so every size check passes) but is
    // not BGRA8, which the BGRA8-only input locks refuse.
    UTexture2D* MakeG8SourceTexture(const FString& PkgPath, int32 N)
    {
        UTexture2D* Texture = MakeConstantSourceTexture(PkgPath, N, 0, 0, 0, 255, /*bWithSource=*/false);
        if (Texture)
        {
            TArray<uint8> Gray;
            Gray.Init(128, N * N);
            Texture->Source.Init(N, N, 1, 1, TSF_G8, Gray.GetData());
        }
        return Texture;
    }

    // Runs Verb on an input the verb refuses and asserts the refusal left no output asset at
    // <Folder>/<OutName>: the input check must run before the output is created, or the orphan
    // output blocks a retry under the same name with ASSET_ALREADY_EXISTS.
    void ExpectRefusedInputLeavesNoOutput(FAutomationTestBase& Test, const TCHAR* Verb,
        const TSharedPtr<FJsonObject>& Payload, const FString& OutPkg)
    {
        ON_SCOPE_EXIT
        {
            if (UObject* Out = StaticFindObject(UTexture2D::StaticClass(), nullptr, *ToObjectPath(OutPkg)))
            {
                Out->GetOutermost()->SetDirtyFlag(false);
            }
        };
        FTestResponseCapture Capture;
        if (!Test.TestTrue(FString::Printf(TEXT("%s handler registered"), Verb),
                InvokeHandlerWithCapture(Verb, Payload, Capture)))
        {
            return;
        }
        Test.TestFalse(FString::Printf(TEXT("%s refuses the unsupported input"), Verb), Capture.bSuccess);
        Test.TestNull(FString::Printf(TEXT("%s refused on its input leaves no output asset (message: %s)"),
                          Verb, *Capture.Message),
            StaticFindObject(UTexture2D::StaticClass(), nullptr, *ToObjectPath(OutPkg)));
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCombineTexturesOutputNamingAnInputIsRefusedTest,
    "PinWright.texture.combine_textures.OutputNamingAnInputIsRefused",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCombineTexturesOutputNamingAnInputIsRefusedTest::RunTest(const FString& Parameters)
{
    using namespace CombineTexturesOutputGuardTestHelpers;
    const int32 N = 16;
    const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString Folder = TEXT("/Game/PinWrightTests");
    const FString BaseName = FString::Printf(TEXT("T_GuardBase_%s"), *Suffix);
    const FString BasePkg = FString::Printf(TEXT("%s/%s"), *Folder, *BaseName);
    const FString OverlayPkg = FString::Printf(TEXT("%s/T_GuardOvl_%s"), *Folder, *Suffix);

    UTexture2D* BaseTex = MakeConstantSourceTexture(BasePkg, N, 40, 80, 120, 255);
    UTexture2D* OverlayTex = MakeConstantSourceTexture(OverlayPkg, N, 255, 255, 255, 255);
    if (!TestNotNull(TEXT("base texture created"), BaseTex) ||
        !TestNotNull(TEXT("overlay texture created"), OverlayTex))
    {
        return true;
    }
    ON_SCOPE_EXIT
    {
        if (BaseTex) { BaseTex->GetOutermost()->SetDirtyFlag(false); }
        if (OverlayTex) { OverlayTex->GetOutermost()->SetDirtyFlag(false); }
    };
    uint8 Before[4];
    if (!TestTrue(TEXT("precondition: base source readable"), ReadFirstSourcePixel(BaseTex, Before)) ||
        !TestTrue(TEXT("precondition: base holds the fixture colour"), Before[0] == 40 && Before[1] == 80 && Before[2] == 120))
    {
        return true;
    }

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("baseTexture"), ToObjectPath(BasePkg));
    Payload->SetStringField(TEXT("overlayTexture"), ToObjectPath(OverlayPkg));
    Payload->SetStringField(TEXT("blendMode"), TEXT("Multiply"));
    Payload->SetStringField(TEXT("name"), BaseName);
    Payload->SetStringField(TEXT("path"), Folder);
    Payload->SetBoolField(TEXT("save"), false);

    FTestResponseCapture Capture;
    if (!TestTrue(TEXT("combine_textures handler registered"),
            InvokeHandlerWithCapture(TEXT("texture.combine_textures"), Payload, Capture)))
    {
        return true;
    }
    TestFalse(TEXT("an output naming the base is refused"), Capture.bSuccess);
    TestEqual(*FString::Printf(TEXT("refused with ASSET_ALREADY_EXISTS (message: %s)"), *Capture.Message),
        Capture.ErrorCode, FString(TEXT("ASSET_ALREADY_EXISTS")));

    uint8 After[4];
    if (TestTrue(TEXT("base source still readable"), ReadFirstSourcePixel(BaseTex, After)))
    {
        TestTrue(*FString::Printf(TEXT("base pixels unchanged (B=%d G=%d R=%d A=%d, expected 40/80/120/255)"),
                     static_cast<int32>(After[0]), static_cast<int32>(After[1]),
                     static_cast<int32>(After[2]), static_cast<int32>(After[3])),
            FMemory::Memcmp(Before, After, 4) == 0);
    }

    // The refusal returns while the base and overlay are read-locked; an in-place invert needs a
    // ReadWrite lock on the base, which LockMipInternal refuses while a read lock is left behind.
    TSharedPtr<FJsonObject> InvertPayload = MakeShared<FJsonObject>();
    InvertPayload->SetStringField(TEXT("assetPath"), ToObjectPath(BasePkg));
    InvertPayload->SetBoolField(TEXT("inPlace"), true);
    InvertPayload->SetBoolField(TEXT("save"), false);
    FTestResponseCapture InvertCapture;
    if (TestTrue(TEXT("invert handler registered"),
            InvokeHandlerWithCapture(TEXT("texture.invert"), InvertPayload, InvertCapture)))
    {
        TestTrue(*FString::Printf(TEXT("base source is unlocked after the refusal (invert err='%s': %s)"),
                     *InvertCapture.ErrorCode, *InvertCapture.Message),
            InvertCapture.bSuccess);
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCreateNoiseTextureExistingOutputIsRefusedTest,
    "PinWright.texture.create_noise_texture.ExistingOutputIsRefused",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCreateNoiseTextureExistingOutputIsRefusedTest::RunTest(const FString& Parameters)
{
    using namespace CombineTexturesOutputGuardTestHelpers;
    const int32 N = 16;
    const FString Folder = TEXT("/Game/PinWrightTests");
    const FString Name = FString::Printf(TEXT("T_GuardNoise_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    const FString Pkg = FString::Printf(TEXT("%s/%s"), *Folder, *Name);

    UTexture2D* Existing = MakeConstantSourceTexture(Pkg, N, 10, 20, 30, 255);
    if (!TestNotNull(TEXT("existing texture created"), Existing))
    {
        return true;
    }
    ON_SCOPE_EXIT
    {
        if (Existing) { Existing->GetOutermost()->SetDirtyFlag(false); }
    };
    uint8 Before[4];
    if (!TestTrue(TEXT("precondition: existing source readable"), ReadFirstSourcePixel(Existing, Before)))
    {
        return true;
    }

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("name"), Name);
    Payload->SetStringField(TEXT("path"), Folder);
    Payload->SetNumberField(TEXT("width"), 32);
    Payload->SetNumberField(TEXT("height"), 32);
    Payload->SetBoolField(TEXT("save"), false);

    FTestResponseCapture Capture;
    if (!TestTrue(TEXT("create_noise_texture handler registered"),
            InvokeHandlerWithCapture(TEXT("texture.create_noise_texture"), Payload, Capture)))
    {
        return true;
    }
    TestFalse(TEXT("an existing texture at the output is refused, not replaced"), Capture.bSuccess);
    TestEqual(*FString::Printf(TEXT("refused with ASSET_ALREADY_EXISTS (message: %s)"), *Capture.Message),
        Capture.ErrorCode, FString(TEXT("ASSET_ALREADY_EXISTS")));

    TestEqual(TEXT("existing texture keeps its size"), static_cast<int32>(Existing->Source.GetSizeX()), N);
    uint8 After[4];
    if (TestTrue(TEXT("existing source still readable"), ReadFirstSourcePixel(Existing, After)))
    {
        TestTrue(TEXT("existing pixels unchanged"), FMemory::Memcmp(Before, After, 4) == 0);
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FResizeTextureRefusedInputLeavesNoOutputTest,
    "PinWright.texture.resize_texture.RefusedInputLeavesNoOutput",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FResizeTextureRefusedInputLeavesNoOutputTest::RunTest(const FString& Parameters)
{
    const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString Folder = TEXT("/Game/PinWrightTests");
    const FString SrcPkg = FString::Printf(TEXT("%s/T_GuardResizeSrc_%s"), *Folder, *Suffix);
    const FString OutName = FString::Printf(TEXT("T_GuardResizeOut_%s"), *Suffix);

    // No editor source: the resize source lock refuses it.
    UTexture2D* Source = MakeConstantSourceTexture(SrcPkg, 16, 0, 0, 0, 255, /*bWithSource=*/false);
    if (!TestNotNull(TEXT("sourceless input created"), Source))
    {
        return true;
    }
    ON_SCOPE_EXIT { if (Source) { Source->GetOutermost()->SetDirtyFlag(false); } };
    TestFalse(TEXT("precondition: input has no editor source"), Source->Source.IsValid());

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("sourcePath"), ToObjectPath(SrcPkg));
    Payload->SetStringField(TEXT("name"), OutName);
    Payload->SetStringField(TEXT("path"), Folder);
    Payload->SetNumberField(TEXT("newWidth"), 8);
    Payload->SetNumberField(TEXT("newHeight"), 8);
    Payload->SetBoolField(TEXT("save"), false);
    CombineTexturesOutputGuardTestHelpers::ExpectRefusedInputLeavesNoOutput(
        *this, TEXT("texture.resize_texture"), Payload, FString::Printf(TEXT("%s/%s"), *Folder, *OutName));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FInvertCopyRefusedInputLeavesNoOutputTest,
    "PinWright.texture.invert.RefusedCopyInputLeavesNoOutput",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FInvertCopyRefusedInputLeavesNoOutputTest::RunTest(const FString& Parameters)
{
    const int32 N = 16;
    const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString Folder = TEXT("/Game/PinWrightTests");
    const FString SrcPkg = FString::Printf(TEXT("%s/T_GuardInvSrc_%s"), *Folder, *Suffix);
    const FString OutName = FString::Printf(TEXT("T_GuardInvOut_%s"), *Suffix);

    UTexture2D* Source = CombineTexturesOutputGuardTestHelpers::MakeG8SourceTexture(SrcPkg, N);
    if (!TestNotNull(TEXT("input created"), Source))
    {
        return true;
    }
    ON_SCOPE_EXIT { if (Source) { Source->GetOutermost()->SetDirtyFlag(false); } };
    if (!TestEqual(TEXT("precondition: input source is G8"), static_cast<int32>(Source->Source.GetFormat()),
            static_cast<int32>(TSF_G8)))
    {
        return true;
    }

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), ToObjectPath(SrcPkg));
    Payload->SetBoolField(TEXT("inPlace"), false);
    Payload->SetStringField(TEXT("name"), OutName);
    Payload->SetStringField(TEXT("path"), Folder);
    Payload->SetBoolField(TEXT("save"), false);
    CombineTexturesOutputGuardTestHelpers::ExpectRefusedInputLeavesNoOutput(
        *this, TEXT("texture.invert"), Payload, FString::Printf(TEXT("%s/%s"), *Folder, *OutName));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDesaturateCopyRefusedInputLeavesNoOutputTest,
    "PinWright.texture.desaturate.RefusedCopyInputLeavesNoOutput",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDesaturateCopyRefusedInputLeavesNoOutputTest::RunTest(const FString& Parameters)
{
    const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString Folder = TEXT("/Game/PinWrightTests");
    const FString SrcPkg = FString::Printf(TEXT("%s/T_GuardDesatSrc_%s"), *Folder, *Suffix);
    const FString OutName = FString::Printf(TEXT("T_GuardDesatOut_%s"), *Suffix);

    UTexture2D* Source = CombineTexturesOutputGuardTestHelpers::MakeG8SourceTexture(SrcPkg, 16);
    if (!TestNotNull(TEXT("G8 input created"), Source))
    {
        return true;
    }
    ON_SCOPE_EXIT { if (Source) { Source->GetOutermost()->SetDirtyFlag(false); } };
    if (!TestEqual(TEXT("precondition: input source is G8"), static_cast<int32>(Source->Source.GetFormat()),
            static_cast<int32>(TSF_G8)))
    {
        return true;
    }

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), ToObjectPath(SrcPkg));
    Payload->SetBoolField(TEXT("inPlace"), false);
    Payload->SetStringField(TEXT("name"), OutName);
    Payload->SetStringField(TEXT("path"), Folder);
    Payload->SetBoolField(TEXT("save"), false);
    CombineTexturesOutputGuardTestHelpers::ExpectRefusedInputLeavesNoOutput(
        *this, TEXT("texture.desaturate"), Payload, FString::Printf(TEXT("%s/%s"), *Folder, *OutName));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAdjustCurvesCopyRefusedInputLeavesNoOutputTest,
    "PinWright.texture.adjust_curves.RefusedCopyInputLeavesNoOutput",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAdjustCurvesCopyRefusedInputLeavesNoOutputTest::RunTest(const FString& Parameters)
{
    const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString Folder = TEXT("/Game/PinWrightTests");
    const FString SrcPkg = FString::Printf(TEXT("%s/T_GuardCurvesSrc_%s"), *Folder, *Suffix);
    const FString OutName = FString::Printf(TEXT("T_GuardCurvesOut_%s"), *Suffix);

    UTexture2D* Source = CombineTexturesOutputGuardTestHelpers::MakeG8SourceTexture(SrcPkg, 16);
    if (!TestNotNull(TEXT("G8 input created"), Source))
    {
        return true;
    }
    ON_SCOPE_EXIT { if (Source) { Source->GetOutermost()->SetDirtyFlag(false); } };
    if (!TestEqual(TEXT("precondition: input source is G8"), static_cast<int32>(Source->Source.GetFormat()),
            static_cast<int32>(TSF_G8)))
    {
        return true;
    }

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), ToObjectPath(SrcPkg));
    Payload->SetBoolField(TEXT("inPlace"), false);
    Payload->SetStringField(TEXT("name"), OutName);
    Payload->SetStringField(TEXT("path"), Folder);
    Payload->SetBoolField(TEXT("save"), false);
    CombineTexturesOutputGuardTestHelpers::ExpectRefusedInputLeavesNoOutput(
        *this, TEXT("texture.adjust_curves"), Payload, FString::Printf(TEXT("%s/%s"), *Folder, *OutName));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FChannelPackRefusedInputLeavesNoOutputTest,
    "PinWright.texture.channel_pack.RefusedInputLeavesNoOutput",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FChannelPackRefusedInputLeavesNoOutputTest::RunTest(const FString& Parameters)
{
    const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString Folder = TEXT("/Game/PinWrightTests");
    const FString SrcPkg = FString::Printf(TEXT("%s/T_GuardPackSrc_%s"), *Folder, *Suffix);
    const FString OutName = FString::Printf(TEXT("T_GuardPackOut_%s"), *Suffix);

    UTexture2D* Source = CombineTexturesOutputGuardTestHelpers::MakeG8SourceTexture(SrcPkg, 16);
    if (!TestNotNull(TEXT("G8 input created"), Source))
    {
        return true;
    }
    ON_SCOPE_EXIT { if (Source) { Source->GetOutermost()->SetDirtyFlag(false); } };
    if (!TestEqual(TEXT("precondition: input source is G8"), static_cast<int32>(Source->Source.GetFormat()),
            static_cast<int32>(TSF_G8)))
    {
        return true;
    }

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    // Red is the first supplied channel, so it also sets the output size: a G8 source passes the
    // size check and is refused only by the BGRA8 channel read.
    Payload->SetStringField(TEXT("redTexture"), ToObjectPath(SrcPkg));
    Payload->SetStringField(TEXT("name"), OutName);
    Payload->SetStringField(TEXT("path"), Folder);
    Payload->SetBoolField(TEXT("save"), false);
    CombineTexturesOutputGuardTestHelpers::ExpectRefusedInputLeavesNoOutput(
        *this, TEXT("texture.channel_pack"), Payload, FString::Printf(TEXT("%s/%s"), *Folder, *OutName));
    return true;
}

// A never-saved package holding a live asset under ANOTHER name: CreatePackage would return it and
// NewObject would add a second asset to it, so the output name (the package leaf) is refused. Once
// that asset is deleted (garbage, not yet collected) the same name is accepted again.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCreateNoiseTexturePackageHoldingOtherAssetIsRefusedTest,
    "PinWright.texture.create_noise_texture.PackageHoldingOtherAssetIsRefused",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCreateNoiseTexturePackageHoldingOtherAssetIsRefusedTest::RunTest(const FString& Parameters)
{
    const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString Folder = TEXT("/Game/PinWrightTests");
    const FString Name = FString::Printf(TEXT("T_GuardPkg_%s"), *Suffix);
    const FString Pkg = FString::Printf(TEXT("%s/%s"), *Folder, *Name);

    UPackage* Package = CreatePackage(*Pkg);
    if (!TestNotNull(TEXT("package created"), Package))
    {
        return true;
    }
    UTexture2D* Other = NewObject<UTexture2D>(Package, *FString::Printf(TEXT("Other_%s"), *Suffix),
        RF_Public | RF_Standalone);
    ON_SCOPE_EXIT { Package->SetDirtyFlag(false); };
    if (!TestNotNull(TEXT("other-named asset created"), Other) ||
        !TestTrue(TEXT("precondition: the package reports the other-named asset"),
            Package->FindAssetInPackage() == Other) ||
        !TestNull(TEXT("precondition: nothing exists under the output name"),
            StaticFindObject(UObject::StaticClass(), nullptr, *ToObjectPath(Pkg))))
    {
        return true;
    }

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("name"), Name);
    Payload->SetStringField(TEXT("path"), Folder);
    Payload->SetNumberField(TEXT("width"), 32);
    Payload->SetNumberField(TEXT("height"), 32);
    Payload->SetBoolField(TEXT("save"), false);

    FTestResponseCapture Capture;
    if (!TestTrue(TEXT("create_noise_texture handler registered"),
            InvokeHandlerWithCapture(TEXT("texture.create_noise_texture"), Payload, Capture)))
    {
        return true;
    }
    TestFalse(TEXT("a package holding another live asset is refused"), Capture.bSuccess);
    TestEqual(*FString::Printf(TEXT("refused with ASSET_ALREADY_EXISTS (message: %s)"), *Capture.Message),
        Capture.ErrorCode, FString(TEXT("ASSET_ALREADY_EXISTS")));
    TestNull(TEXT("no second asset was added to the package"),
        StaticFindObject(UObject::StaticClass(), nullptr, *ToObjectPath(Pkg)));

    // Delete the other asset the way asset deletion does (drop RF_Standalone, mark garbage); until
    // GC runs it is still in the package but no longer a valid asset, so it must not block the name.
    Other->ClearFlags(RF_Standalone | RF_Public);
    Other->MarkAsGarbage();
    FTestResponseCapture Retry;
    if (TestTrue(TEXT("create_noise_texture handler registered (retry)"),
            InvokeHandlerWithCapture(TEXT("texture.create_noise_texture"), Payload, Retry)))
    {
        TestTrue(*FString::Printf(TEXT("retry after deleting the other asset succeeds (err='%s': %s)"),
                     *Retry.ErrorCode, *Retry.Message),
            Retry.bSuccess);
    }
    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
