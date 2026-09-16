// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for the texture pixel-content readback (ticket F-texture-pixel-stats-readback).
//
// Before this fix every live texture read RPC (texture.describe / texture.get_texture_info)
// returned metadata only, so a caller could not confirm that desaturate / invert /
// adjust_levels actually changed the pixels in the intended direction — success was inferred
// from a bare success string plus a re-encoded sizeBytes delta. TexturePixelStats::BuildPixelStatsJson
// reads the editable source mip (the same FTextureSource::LockMip access the mutating verbs use)
// and reports per-channel mean/min/max, a grayscale flag, a maxChannelSpread, and a content hash.
//
// Counterfactual: reverting the fix removes BuildPixelStatsJson, so this test fails to compile/link.
// With the fix present, mis-reading the BGRA byte order, the grayscale criterion, or the
// channel aggregates fails the assertions below.
#include "Misc/AutomationTest.h"

#include "Handlers/Asset/TexturePixelStats.h"

#include "Dom/JsonObject.h"
#include "Engine/Texture.h"
#include "Engine/Texture2D.h"
#include "UObject/Package.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
    // Appends one BGRA8 source pixel (note: source byte order is B,G,R,A).
    void PushBGRA(TArray<uint8>& Bytes, uint8 R, uint8 G, uint8 B, uint8 A)
    {
        Bytes.Add(B);
        Bytes.Add(G);
        Bytes.Add(R);
        Bytes.Add(A);
    }

    UTexture2D* MakeBGRASourceTexture(int32 Width, int32 Height, const TArray<uint8>& Bytes)
    {
        UTexture2D* Tex = NewObject<UTexture2D>(GetTransientPackage());
        if (Tex)
        {
            Tex->Source.Init(Width, Height, 1, 1, TSF_BGRA8, Bytes.GetData());
        }
        return Tex;
    }

    double ChannelValue(const TSharedPtr<FJsonObject>& Root, const TCHAR* Block, const TCHAR* Channel)
    {
        const TSharedPtr<FJsonObject>* Obj = nullptr;
        if (Root.IsValid() && Root->TryGetObjectField(Block, Obj) && Obj && Obj->IsValid())
        {
            return (*Obj)->GetNumberField(Channel);
        }
        return -1.0;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTexturePixelStatsReadbackTest,
    "PinWright.texture.get_pixel_stats.SourceMipStats",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FTexturePixelStatsReadbackTest::RunTest(const FString& Parameters)
{
    // --- Colored 2x2 image: R/G/B differ in every pixel, so it is NOT grayscale. ---
    // Channel means: R = (10+40+70+100)/4 = 55, G = (20+50+80+110)/4 = 65,
    //                B = (30+60+90+120)/4 = 75, A = 255. Max per-pixel spread = 20.
    TArray<uint8> Colored;
    PushBGRA(Colored, 10, 20, 30, 255);
    PushBGRA(Colored, 40, 50, 60, 255);
    PushBGRA(Colored, 70, 80, 90, 255);
    PushBGRA(Colored, 100, 110, 120, 255);

    UTexture2D* ColoredTex = MakeBGRASourceTexture(2, 2, Colored);
    if (!TestNotNull(TEXT("Constructed colored source texture"), ColoredTex))
    {
        return true;
    }

    FString Error;
    TSharedPtr<FJsonObject> Stats = TexturePixelStats::BuildPixelStatsJson(ColoredTex, 0, Error);
    if (!TestTrue(FString::Printf(TEXT("Colored stats built (err='%s')"), *Error), Stats.IsValid()))
    {
        return true;
    }

    TestEqual(TEXT("width"), static_cast<int32>(Stats->GetNumberField(TEXT("width"))), 2);
    TestEqual(TEXT("height"), static_cast<int32>(Stats->GetNumberField(TEXT("height"))), 2);
    TestEqual(TEXT("pixelCount"), static_cast<int32>(Stats->GetNumberField(TEXT("pixelCount"))), 4);
    TestEqual(TEXT("sourceFormat"), Stats->GetStringField(TEXT("sourceFormat")), FString(TEXT("TSF_BGRA8")));

    // BGRA byte order must be decoded correctly: a swap of R/B would flip these means.
    TestEqual(TEXT("mean.r"), ChannelValue(Stats, TEXT("mean"), TEXT("r")), 55.0);
    TestEqual(TEXT("mean.g"), ChannelValue(Stats, TEXT("mean"), TEXT("g")), 65.0);
    TestEqual(TEXT("mean.b"), ChannelValue(Stats, TEXT("mean"), TEXT("b")), 75.0);
    TestEqual(TEXT("mean.a"), ChannelValue(Stats, TEXT("mean"), TEXT("a")), 255.0);

    TestEqual(TEXT("min.r"), ChannelValue(Stats, TEXT("min"), TEXT("r")), 10.0);
    TestEqual(TEXT("max.r"), ChannelValue(Stats, TEXT("max"), TEXT("r")), 100.0);
    TestEqual(TEXT("max.b"), ChannelValue(Stats, TEXT("max"), TEXT("b")), 120.0);

    TestEqual(TEXT("maxChannelSpread"),
        static_cast<int32>(Stats->GetNumberField(TEXT("maxChannelSpread"))), 20);
    bool bGray = true;
    TestTrue(TEXT("grayscale field present"), Stats->TryGetBoolField(TEXT("grayscale"), bGray));
    TestFalse(TEXT("colored image is not grayscale"), bGray);
    TestTrue(TEXT("hash present"), Stats->HasField(TEXT("hash")));
    const FString ColoredHash = Stats->GetStringField(TEXT("hash"));

    // --- Grayscale 2x2 image: R==G==B in every pixel → grayscale=true, spread=0. ---
    TArray<uint8> Gray;
    PushBGRA(Gray, 50, 50, 50, 255);
    PushBGRA(Gray, 100, 100, 100, 255);
    PushBGRA(Gray, 150, 150, 150, 255);
    PushBGRA(Gray, 200, 200, 200, 255);

    UTexture2D* GrayTex = MakeBGRASourceTexture(2, 2, Gray);
    if (!TestNotNull(TEXT("Constructed grayscale source texture"), GrayTex))
    {
        return true;
    }

    TSharedPtr<FJsonObject> GrayStats = TexturePixelStats::BuildPixelStatsJson(GrayTex, 0, Error);
    if (!TestTrue(FString::Printf(TEXT("Gray stats built (err='%s')"), *Error), GrayStats.IsValid()))
    {
        return true;
    }

    TestEqual(TEXT("gray maxChannelSpread is 0"),
        static_cast<int32>(GrayStats->GetNumberField(TEXT("maxChannelSpread"))), 0);
    bGray = false;
    TestTrue(TEXT("grayscale field present (gray)"), GrayStats->TryGetBoolField(TEXT("grayscale"), bGray));
    TestTrue(TEXT("R==G==B image is grayscale"), bGray);
    TestEqual(TEXT("gray mean.r"), ChannelValue(GrayStats, TEXT("mean"), TEXT("r")), 125.0);
    TestEqual(TEXT("gray mean.b"), ChannelValue(GrayStats, TEXT("mean"), TEXT("b")), 125.0);

    // Distinct images must hash differently — proves the hash reflects pixel content, the
    // signal that lets a caller detect "did the pixels actually change at all".
    TestNotEqual(TEXT("colored and gray hashes differ"),
        ColoredHash, GrayStats->GetStringField(TEXT("hash")));

    // --- Unsupported source format fails loud rather than misreading the byte layout. ---
    UTexture2D* FloatTex = NewObject<UTexture2D>(GetTransientPackage());
    if (TestNotNull(TEXT("Constructed float source texture"), FloatTex))
    {
        FloatTex->Source.Init(2, 2, 1, 1, TSF_RGBA16F);
        FString FloatError;
        TSharedPtr<FJsonObject> FloatStats = TexturePixelStats::BuildPixelStatsJson(FloatTex, 0, FloatError);
        TestFalse(TEXT("RGBA16F source yields no stats"), FloatStats.IsValid());
        TestTrue(TEXT("unsupported-format error is populated"), !FloatError.IsEmpty());
    }

    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
