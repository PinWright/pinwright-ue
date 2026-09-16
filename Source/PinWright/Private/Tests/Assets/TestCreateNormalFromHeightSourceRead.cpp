// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for board B-texture-normal-from-height-crash.
//
// texture.create_normal_from_height used to read the source height texture's *built
// platform mip* (GetPlatformData()->Mips[0].BulkData.LockReadOnly()) and index the
// result without a null check. For a freshly built / compressed source (e.g. one just
// created by texture.create_pattern_texture / create_gradient_texture, encoded
// TFO_AutoDXT) that platform mip is GPU-only, so LockReadOnly() returned nullptr and the
// pixel loop dereferenced address 0x0 — a hard EXCEPTION_ACCESS_VIOLATION that took down
// the whole editor. The fix reads from the editor FTextureSource instead (always
// CPU-resident and uncompressed) and honors the real source format.
//
// This test builds a source UTexture2D whose editor Source carries a strong horizontal
// "tent" height gradient while its platform mip is left resident-but-FLAT (all zeros). It
// then drives the production handler and reads back the output normal map:
//   * With the fix (reads Source): the gradient yields a normal map whose X channel
//     (encoded into R) varies widely across the image.
//   * Reverting the fix (reads the flat platform mip): every height is 0, so the normal
//     map is uniformly (0,0,1) -> a flat R ~= 127 with zero spread, failing the assertion.
// The flat-but-resident platform mip is what keeps a revert from crashing this test
// process (LockReadOnly returns a valid pointer to zeros rather than null), so the
// regression surfaces as a clean assertion failure, not a suite-killing access violation.
#include "Misc/AutomationTest.h"

#include "Dom/JsonObject.h"
#include "Engine/Texture.h"
#include "Engine/Texture2D.h"
#include "PixelFormat.h"
#include "TextureResource.h"
#include "Tests/TestUtils.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCreateNormalFromHeightReadsEditorSourceTest,
    "PinWright.texture.create_normal_from_height.ReadsFromEditorSource",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCreateNormalFromHeightReadsEditorSourceTest::RunTest(const FString& Parameters)
{
    const int32 N = 16;

    // A named /Game package (NOT the transient package) so the handler's path-based
    // StaticLoadObject resolves the source. GUID-suffixed so parallel/repeat runs never
    // collide.
    const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString SrcPkgPath = FString::Printf(TEXT("/Game/PinWrightTests/T_NFHSrc_%s"), *Suffix);
    const FString SrcAssetName = FPackageName::GetLongPackageAssetName(SrcPkgPath);
    const FString OutFolder = TEXT("/Game/PinWrightTests");
    const FString OutName = FString::Printf(TEXT("T_NFHNrm_%s"), *Suffix);
    const FString OutPkgPath = FString::Printf(TEXT("%s/%s"), *OutFolder, *OutName);

    UPackage* SrcPackage = CreatePackage(*SrcPkgPath);
    if (!TestNotNull(TEXT("source package created"), SrcPackage))
    {
        return true;
    }
    ON_SCOPE_EXIT
    {
        // save:false leaves both assets in-memory only; clear dirty so the disposable host
        // is not left carrying unsaved packages.
        if (SrcPackage) { SrcPackage->SetDirtyFlag(false); }
        if (UObject* Out = StaticFindObject(UTexture2D::StaticClass(), nullptr, *ToObjectPath(OutPkgPath)))
        {
            if (UPackage* OutPkg = Out->GetOutermost()) { OutPkg->SetDirtyFlag(false); }
        }
    };

    UTexture2D* HeightTex = NewObject<UTexture2D>(
        SrcPackage, *SrcAssetName, RF_Public | RF_Standalone);
    if (!TestNotNull(TEXT("source height texture created"), HeightTex))
    {
        return true;
    }

    // Build a resident-but-FLAT BGRA8 platform mip. This is only exercised by the (buggy)
    // pre-fix code path; keeping it resident and readable means a revert reads flat zeros
    // instead of null-dereferencing, so this test fails cleanly rather than crashing.
    HeightTex->SetPlatformData(new FTexturePlatformData());
    HeightTex->GetPlatformData()->SizeX = N;
    HeightTex->GetPlatformData()->SizeY = N;
    HeightTex->GetPlatformData()->PixelFormat = PF_B8G8R8A8;
    FTexture2DMipMap* Mip = new FTexture2DMipMap();
    Mip->SizeX = N;
    Mip->SizeY = N;
    HeightTex->GetPlatformData()->Mips.Add(Mip);
    {
        const int64 FlatBytes = static_cast<int64>(N) * N * 4;
        Mip->BulkData.Lock(LOCK_READ_WRITE);
        void* Dst = Mip->BulkData.Realloc(FlatBytes);
        FMemory::Memzero(Dst, FlatBytes);
        Mip->BulkData.Unlock();
    }

    // Build the editor Source (BGRA8) with a horizontal "tent" height profile: brightness
    // rises to the middle then falls, so the normal's X component swings from strongly
    // negative on the left to strongly positive on the right.
    TArray<uint8> SrcBytes;
    SrcBytes.SetNumUninitialized(N * N * 4);
    for (int32 Y = 0; Y < N; ++Y)
    {
        for (int32 X = 0; X < N; ++X)
        {
            const int32 Dist = (X < N / 2) ? X : (N - 1 - X);
            const uint8 V = static_cast<uint8>((Dist * 255) / (N / 2));
            const int32 Idx = (Y * N + X) * 4;
            SrcBytes[Idx + 0] = V;   // B
            SrcBytes[Idx + 1] = V;   // G
            SrcBytes[Idx + 2] = V;   // R
            SrcBytes[Idx + 3] = 255; // A
        }
    }
    HeightTex->Source.Init(N, N, 1, 1, TSF_BGRA8, SrcBytes.GetData());
    if (!TestTrue(TEXT("source has editor source data"), HeightTex->Source.IsValid()))
    {
        return true;
    }

    // Drive the production handler through the real dispatch path.
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("sourceTexture"), ToObjectPath(SrcPkgPath));
    Payload->SetStringField(TEXT("name"), OutName);
    Payload->SetStringField(TEXT("path"), OutFolder);
    Payload->SetStringField(TEXT("algorithm"), TEXT("Sobel"));
    Payload->SetStringField(TEXT("channelMode"), TEXT("luminance"));
    Payload->SetBoolField(TEXT("save"), false);

    FTestResponseCapture Capture;
    if (!TestTrue(TEXT("create_normal_from_height handler registered"),
            InvokeHandlerWithCapture(TEXT("texture.create_normal_from_height"), Payload, Capture)))
    {
        return true;
    }
    TestTrue(*FString::Printf(TEXT("handler succeeds (err='%s': %s)"),
                 *Capture.ErrorCode, *Capture.Message),
        Capture.bSuccess);
    if (!Capture.bSuccess)
    {
        return true;
    }

    // Read the produced normal map back from its editor Source (BGRA8). The X normal is
    // encoded into R (byte index 2).
    UTexture2D* NormalMap = Cast<UTexture2D>(
        StaticFindObject(UTexture2D::StaticClass(), nullptr, *ToObjectPath(OutPkgPath)));
    if (!TestNotNull(TEXT("output normal map exists"), NormalMap))
    {
        return true;
    }
    if (!TestTrue(TEXT("output normal map has source data"), NormalMap->Source.IsValid()))
    {
        return true;
    }
    TestEqual(TEXT("output width matches source"), static_cast<int32>(NormalMap->Source.GetSizeX()), N);
    TestEqual(TEXT("output height matches source"), static_cast<int32>(NormalMap->Source.GetSizeY()), N);

    const uint8* OutPixels = NormalMap->Source.LockMipReadOnly(0);
    if (!TestNotNull(TEXT("output source mip readable"), OutPixels))
    {
        return true;
    }
    uint8 MinR = 255;
    uint8 MaxR = 0;
    for (int32 i = 0; i < N * N; ++i)
    {
        const uint8 R = OutPixels[i * 4 + 2];
        MinR = FMath::Min(MinR, R);
        MaxR = FMath::Max(MaxR, R);
    }
    NormalMap->Source.UnlockMip(0);

    // With the fix the gradient produces a wide X-normal spread; reverting to the flat
    // platform-mip read collapses every normal to (0,0,1) -> R uniform ~127, spread 0.
    TestTrue(
        *FString::Printf(TEXT("normal map X channel varies (read from editor source, not flat platform mip): MinR=%d MaxR=%d"),
            static_cast<int32>(MinR), static_cast<int32>(MaxR)),
        (static_cast<int32>(MaxR) - static_cast<int32>(MinR)) > 8);

    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
