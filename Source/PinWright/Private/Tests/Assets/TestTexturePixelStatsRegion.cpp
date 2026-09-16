// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for sub-region / tile-grid pixel stats (ticket F-texture-pixel-stats-region).
//
// texture.get_pixel_stats used to cover the whole mip or nothing, so every question of the form
// "what is in THIS part of the image" was unanswerable through the plugin — which is the entire
// class of tiled authoring textures (SubUV flipbook atlases, sprite sheets, row-banded mask
// sheets), where the whole-image mean is close to meaningless.
//
// The fixture is a 4x4 image of four flat 2x2 quadrants with distinct values, so every assertion
// below has a different expected number from the whole-mip answer.
//
// Counterfactual: revert the region/tileGrid support in TexturePixelStats.cpp and the region
// stats collapse to the whole-mip values — mean.r over the top-left quadrant reads 102.5 instead
// of 10, and the per-tile array is absent entirely.
#include "Misc/AutomationTest.h"

#include "Handlers/Asset/TexturePixelStats.h"

#include "Dom/JsonObject.h"
#include "Engine/Texture.h"
#include "Engine/Texture2D.h"
#include "UObject/Package.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
    // A 4x4 BGRA8 image of four flat 2x2 quadrants:
    //   top-left     (10, 20, 30)  - the one non-gray quadrant, so R/G/B order is checked per region
    //   top-right    (80, 80, 80)
    //   bottom-left  (120, 120, 120)
    //   bottom-right (200, 200, 200)
    // Whole-mip means: r 102.5, g 105, b 107.5 — none of which equals any quadrant's mean.
    UTexture2D* MakeQuadrantAtlasTexture()
    {
        TArray<uint8> Bytes;
        Bytes.Reserve(4 * 4 * 4);
        for (int32 Y = 0; Y < 4; ++Y)
        {
            for (int32 X = 0; X < 4; ++X)
            {
                uint8 R = 0, G = 0, B = 0;
                if (Y < 2 && X < 2) { R = 10; G = 20; B = 30; }
                else if (Y < 2) { R = G = B = 80; }
                else if (X < 2) { R = G = B = 120; }
                else { R = G = B = 200; }
                // Source byte order is B,G,R,A.
                Bytes.Add(B);
                Bytes.Add(G);
                Bytes.Add(R);
                Bytes.Add(255);
            }
        }

        UTexture2D* Tex = NewObject<UTexture2D>(GetTransientPackage());
        if (Tex)
        {
            Tex->Source.Init(4, 4, 1, 1, TSF_BGRA8, Bytes.GetData());
        }
        return Tex;
    }

    double RegionStatChannel(const TSharedPtr<FJsonObject>& Block, const TCHAR* StatName, const TCHAR* Channel)
    {
        const TSharedPtr<FJsonObject>* Obj = nullptr;
        if (Block.IsValid() && Block->TryGetObjectField(StatName, Obj) && Obj && Obj->IsValid())
        {
            return (*Obj)->GetNumberField(Channel);
        }
        return -1.0;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTexturePixelStatsRegionTest,
    "PinWright.texture.get_pixel_stats.RegionAndTileGrid",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FTexturePixelStatsRegionTest::RunTest(const FString& Parameters)
{
    UTexture2D* Atlas = MakeQuadrantAtlasTexture();
    if (!TestNotNull(TEXT("Constructed quadrant atlas texture"), Atlas))
    {
        return true;
    }

    FString Error;

    // --- No region, no tile grid: unchanged whole-mip answer, and none of the new fields. ---
    TSharedPtr<FJsonObject> WholeMip = TexturePixelStats::BuildPixelStatsJson(Atlas, 0, Error);
    if (!TestTrue(FString::Printf(TEXT("Whole-mip stats built (err='%s')"), *Error), WholeMip.IsValid()))
    {
        return true;
    }
    TestEqual(TEXT("whole-mip pixelCount"), static_cast<int32>(WholeMip->GetNumberField(TEXT("pixelCount"))), 16);
    TestEqual(TEXT("whole-mip mean.r"), RegionStatChannel(WholeMip, TEXT("mean"), TEXT("r")), 102.5);
    TestEqual(TEXT("whole-mip mean.g"), RegionStatChannel(WholeMip, TEXT("mean"), TEXT("g")), 105.0);
    TestEqual(TEXT("whole-mip mean.b"), RegionStatChannel(WholeMip, TEXT("mean"), TEXT("b")), 107.5);
    TestFalse(TEXT("no 'region' field when none was requested"), WholeMip->HasField(TEXT("region")));
    TestFalse(TEXT("no 'tileGrid' field when none was requested"), WholeMip->HasField(TEXT("tileGrid")));
    TestFalse(TEXT("no 'tiles' field when none was requested"), WholeMip->HasField(TEXT("tiles")));

    // --- Top-left 2x2 region: the one quadrant whose channels differ from each other. ---
    TexturePixelStats::FPixelStatsOptions TopLeft;
    TopLeft.bHasRegion = true;
    TopLeft.Region = TexturePixelStats::FPixelRegion{ 0, 0, 2, 2 };
    TSharedPtr<FJsonObject> TopLeftStats = TexturePixelStats::BuildPixelStatsJson(Atlas, TopLeft, Error);
    if (!TestTrue(FString::Printf(TEXT("Top-left region stats built (err='%s')"), *Error), TopLeftStats.IsValid()))
    {
        return true;
    }
    // Whole-mip would answer 102.5 / 105 / 107.5 here — this is the counterfactual assertion.
    TestEqual(TEXT("region mean.r"), RegionStatChannel(TopLeftStats, TEXT("mean"), TEXT("r")), 10.0);
    TestEqual(TEXT("region mean.g"), RegionStatChannel(TopLeftStats, TEXT("mean"), TEXT("g")), 20.0);
    TestEqual(TEXT("region mean.b"), RegionStatChannel(TopLeftStats, TEXT("mean"), TEXT("b")), 30.0);
    TestEqual(TEXT("region pixelCount"), static_cast<int32>(TopLeftStats->GetNumberField(TEXT("pixelCount"))), 4);
    TestEqual(TEXT("region keeps the mip width"), static_cast<int32>(TopLeftStats->GetNumberField(TEXT("width"))), 4);
    bool bRegionGray = true;
    TestTrue(TEXT("region grayscale field present"), TopLeftStats->TryGetBoolField(TEXT("grayscale"), bRegionGray));
    TestFalse(TEXT("top-left quadrant is not grayscale"), bRegionGray);

    const TSharedPtr<FJsonObject>* RegionEcho = nullptr;
    if (TestTrue(TEXT("region echo present"), TopLeftStats->TryGetObjectField(TEXT("region"), RegionEcho))
        && RegionEcho && RegionEcho->IsValid())
    {
        TestEqual(TEXT("region echo width"), static_cast<int32>((*RegionEcho)->GetNumberField(TEXT("width"))), 2);
        TestEqual(TEXT("region echo height"), static_cast<int32>((*RegionEcho)->GetNumberField(TEXT("height"))), 2);
    }

    // --- Bottom-right 2x2 region: a different answer again from the same texture. ---
    TexturePixelStats::FPixelStatsOptions BottomRight;
    BottomRight.bHasRegion = true;
    BottomRight.Region = TexturePixelStats::FPixelRegion{ 2, 2, 2, 2 };
    TSharedPtr<FJsonObject> BottomRightStats = TexturePixelStats::BuildPixelStatsJson(Atlas, BottomRight, Error);
    if (TestTrue(FString::Printf(TEXT("Bottom-right region stats built (err='%s')"), *Error), BottomRightStats.IsValid()))
    {
        TestEqual(TEXT("bottom-right mean.r"), RegionStatChannel(BottomRightStats, TEXT("mean"), TEXT("r")), 200.0);
        TestEqual(TEXT("bottom-right min.b"), RegionStatChannel(BottomRightStats, TEXT("min"), TEXT("b")), 200.0);
    }

    // --- An oversized region is clamped to the mip, not refused and not read out of bounds. ---
    TexturePixelStats::FPixelStatsOptions Clamped;
    Clamped.bHasRegion = true;
    Clamped.Region = TexturePixelStats::FPixelRegion{ 3, 3, 100, 100 };
    TSharedPtr<FJsonObject> ClampedStats = TexturePixelStats::BuildPixelStatsJson(Atlas, Clamped, Error);
    if (TestTrue(FString::Printf(TEXT("Clamped region stats built (err='%s')"), *Error), ClampedStats.IsValid()))
    {
        TestEqual(TEXT("clamped pixelCount"), static_cast<int32>(ClampedStats->GetNumberField(TEXT("pixelCount"))), 1);
        TestEqual(TEXT("clamped mean.r"), RegionStatChannel(ClampedStats, TEXT("mean"), TEXT("r")), 200.0);
    }

    // --- A region whose origin is off the mip is refused: "0 pixels, mean 0" would read as measured. ---
    TexturePixelStats::FPixelStatsOptions OffImage;
    OffImage.bHasRegion = true;
    OffImage.Region = TexturePixelStats::FPixelRegion{ 10, 0, 2, 2 };
    FString OffImageError;
    TestFalse(TEXT("region origin outside the mip yields no stats"),
        TexturePixelStats::BuildPixelStatsJson(Atlas, OffImage, OffImageError).IsValid());
    TestTrue(TEXT("out-of-range region error is populated"), !OffImageError.IsEmpty());

    // --- 2x2 tile grid: one stats block per quadrant, row-major. ---
    TexturePixelStats::FPixelStatsOptions Grid;
    Grid.bHasTileGrid = true;
    Grid.TileColumns = 2;
    Grid.TileRows = 2;
    TSharedPtr<FJsonObject> GridStats = TexturePixelStats::BuildPixelStatsJson(Atlas, Grid, Error);
    if (!TestTrue(FString::Printf(TEXT("Tile-grid stats built (err='%s')"), *Error), GridStats.IsValid()))
    {
        return true;
    }
    // The whole-image block is still there beside the tiles, so one call answers both questions.
    TestEqual(TEXT("tile-grid keeps the whole-image mean.r"),
        RegionStatChannel(GridStats, TEXT("mean"), TEXT("r")), 102.5);

    const TArray<TSharedPtr<FJsonValue>>* Tiles = nullptr;
    if (TestTrue(TEXT("tiles array present"), GridStats->TryGetArrayField(TEXT("tiles"), Tiles)) && Tiles)
    {
        if (TestEqual(TEXT("tile count"), Tiles->Num(), 4))
        {
            const double ExpectedRed[4] = { 10.0, 80.0, 120.0, 200.0 };
            const int32 ExpectedColumn[4] = { 0, 1, 0, 1 };
            const int32 ExpectedRow[4] = { 0, 0, 1, 1 };
            for (int32 Index = 0; Index < 4; ++Index)
            {
                const TSharedPtr<FJsonObject> Tile = (*Tiles)[Index]->AsObject();
                if (!TestTrue(FString::Printf(TEXT("tile %d is an object"), Index), Tile.IsValid()))
                {
                    continue;
                }
                TestEqual(FString::Printf(TEXT("tile %d column"), Index),
                    static_cast<int32>(Tile->GetNumberField(TEXT("column"))), ExpectedColumn[Index]);
                TestEqual(FString::Printf(TEXT("tile %d row"), Index),
                    static_cast<int32>(Tile->GetNumberField(TEXT("row"))), ExpectedRow[Index]);
                TestEqual(FString::Printf(TEXT("tile %d pixelCount"), Index),
                    static_cast<int32>(Tile->GetNumberField(TEXT("pixelCount"))), 4);
                TestEqual(FString::Printf(TEXT("tile %d mean.r"), Index),
                    RegionStatChannel(Tile, TEXT("mean"), TEXT("r")), ExpectedRed[Index]);
            }
        }
    }

    // --- A grid finer than the pixels it would cover is refused rather than emitting empty tiles. ---
    TexturePixelStats::FPixelStatsOptions TooFine;
    TooFine.bHasTileGrid = true;
    TooFine.TileColumns = 8;
    TooFine.TileRows = 1;
    FString TooFineError;
    TestFalse(TEXT("8-column grid over a 4px-wide mip yields no stats"),
        TexturePixelStats::BuildPixelStatsJson(Atlas, TooFine, TooFineError).IsValid());
    TestTrue(TEXT("too-fine grid error is populated"), !TooFineError.IsEmpty());

    // --- MeasureTileGrid is the C++ entry point the niagara SubUV atlas check measures through. ---
    TArray<TexturePixelStats::FRegionStats> MeasuredTiles;
    FString MeasureError;
    const bool bMeasured = TexturePixelStats::MeasureTileGrid(Atlas, 0, 2, 2, MeasuredTiles, MeasureError);
    if (TestTrue(FString::Printf(TEXT("MeasureTileGrid succeeded (err='%s')"), *MeasureError), bMeasured)
        && TestEqual(TEXT("measured tile count"), MeasuredTiles.Num(), 4))
    {
        TestEqual(TEXT("measured tile 0 luma"), MeasuredTiles[0].MeanLuma(), 20.0);
        TestEqual(TEXT("measured tile 1 luma"), MeasuredTiles[1].MeanLuma(), 80.0);
        TestEqual(TEXT("measured tile 2 luma"), MeasuredTiles[2].MeanLuma(), 120.0);
        TestEqual(TEXT("measured tile 3 luma"), MeasuredTiles[3].MeanLuma(), 200.0);
        TestEqual(TEXT("measured tile 3 origin x"), MeasuredTiles[3].Region.X, 2);
        TestEqual(TEXT("measured tile 3 origin y"), MeasuredTiles[3].Region.Y, 2);
    }

    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
