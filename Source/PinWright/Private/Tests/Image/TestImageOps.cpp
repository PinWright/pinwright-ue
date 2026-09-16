// Copyright (c) 2026 Alexander Penkin. MIT License.

// Unit tests for the pure half of the `image.*` namespace: the slicer, the georeference parser and
// the low-opacity overlay contract. No editor, no RHI, no files - everything runs on synthetic
// bitmaps, so a failure here is a defect in the maths and never in the environment.
//
// The seam test is the load-bearing one, and it is written so it CANNOT restate the rule it is
// checking. Every pixel of the synthetic mosaic carries a colour that encodes its own mosaic
// coordinate, so after cutting, the test recovers "which tile did the slicer actually put pixel
// (x,y) in" from the tile BYTES, and asserts that against
// PinWrightTileGrid::PixelToTilePixel. If the two ever disagree - by a copy of the seam rule
// drifting, or by an off-by-one in the slice - the assertion fails; nothing in this file says
// which side of a seam a pixel belongs to.

#include "Misc/AutomationTest.h"

#include "Handlers/Image/ImageOps.h"
#include "Handlers/Render/BitmapPaint.h"
#include "Handlers/Render/TileGridUtils.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace
{
    // A colour that uniquely identifies one mosaic pixel, for images up to 65536 px. The channels
    // are chosen so no two pixels collide and none of them is pure black (which would also be a
    // plausible "never written" value).
    FColor PwImageTestColorFor(int32 X, int32 Y, int32 Width)
    {
        const int32 Index = Y * Width + X;
        return FColor(
            static_cast<uint8>(Index & 0xFF),
            static_cast<uint8>((Index >> 8) & 0xFF),
            static_cast<uint8>(0x40 + ((Index >> 16) & 0x3F)),
            255);
    }

    bool PwImageTestDecodeColor(const FColor& Color, int32 Width, int32& OutX, int32& OutY)
    {
        if (Color.B < 0x40)
        {
            return false;
        }
        const int32 Index = static_cast<int32>(Color.R) |
            (static_cast<int32>(Color.G) << 8) |
            ((static_cast<int32>(Color.B) - 0x40) << 16);
        OutX = Index % Width;
        OutY = Index / Width;
        return true;
    }

    PinWrightImage::FBitmap PwImageTestMakeIdentityMosaic(int32 Width, int32 Height)
    {
        PinWrightImage::FBitmap Bitmap;
        FString Err;
        PinWrightImage::MakeBitmap(Width, Height, FColor::Black, Bitmap, Err);
        for (int32 Y = 0; Y < Height; ++Y)
        {
            for (int32 X = 0; X < Width; ++X)
            {
                Bitmap.Pixels[Y * Width + X] = PwImageTestColorFor(X, Y, Width);
            }
        }
        return Bitmap;
    }

    // A georeference JSON object for a Width x Height image cut Cols x Rows over a world box.
    TSharedPtr<FJsonObject> PwImageTestGeoreferenceJson(int32 Cols, int32 Rows,
        double MinX, double MinY, double MaxX, double MaxY)
    {
        TSharedPtr<FJsonObject> Min = MakeShared<FJsonObject>();
        Min->SetNumberField(TEXT("x"), MinX);
        Min->SetNumberField(TEXT("y"), MinY);
        Min->SetNumberField(TEXT("z"), 0.0);
        TSharedPtr<FJsonObject> Max = MakeShared<FJsonObject>();
        Max->SetNumberField(TEXT("x"), MaxX);
        Max->SetNumberField(TEXT("y"), MaxY);
        Max->SetNumberField(TEXT("z"), 0.0);

        TSharedPtr<FJsonObject> Geo = MakeShared<FJsonObject>();
        Geo->SetStringField(TEXT("axes"), TEXT("top_down_x_right_y_down"));
        Geo->SetObjectField(TEXT("worldMin"), Min);
        Geo->SetObjectField(TEXT("worldMax"), Max);
        Geo->SetNumberField(TEXT("cols"), Cols);
        Geo->SetNumberField(TEXT("rows"), Rows);
        return Geo;
    }
}

// ============================================================================
// The slice is complete, non-overlapping and byte-exact
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwImageTileRoundTripTest,
    "PinWright.image.tile.RoundTripIsLossless",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FPwImageTileRoundTripTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightImage;
    using namespace PinWrightTileGrid;

    constexpr int32 Width = 12;
    constexpr int32 Height = 6;
    const FBitmap Mosaic = PwImageTestMakeIdentityMosaic(Width, Height);

    FGeoreference Geo;
    FString ErrCode;
    FString ErrMsg;
    if (!TestTrue(TEXT("georeference parses"),
        ParseGeoreference(PwImageTestGeoreferenceJson(3, 2, -600.0, -300.0, 600.0, 300.0),
            Width, Height, /*bImageIsSingleTile=*/false, Geo, ErrCode, ErrMsg)))
    {
        AddError(FString::Printf(TEXT("ParseGeoreference failed: %s / %s"), *ErrCode, *ErrMsg));
        return false;
    }
    TestEqual(TEXT("tile pixel width derived from the image"), Geo.Grid.TilePixelWidth, 4);
    TestEqual(TEXT("tile pixel height derived from the image"), Geo.Grid.TilePixelHeight, 3);

    // Cut, then reassemble at the origins the georeference itself reports.
    FBitmap Rebuilt;
    FString Err;
    TestTrue(TEXT("rebuild canvas allocates"), MakeBitmap(Width, Height, FColor::Black, Rebuilt, Err));

    for (int32 Row = 0; Row < Geo.Grid.Rows; ++Row)
    {
        for (int32 Col = 0; Col < Geo.Grid.Cols; ++Col)
        {
            const FTileIndex Tile(Col, Row);
            FBitmap TileBitmap;
            if (!TestTrue(*FString::Printf(TEXT("tile (%d,%d) cuts"), Col, Row),
                ExtractTile(Mosaic, Geo.Grid, Tile, TileBitmap, Err)))
            {
                AddError(Err);
                return false;
            }
            const FVector2D Origin = TilePixelToPixel(Geo.Grid, Tile, FVector2D::ZeroVector);
            TestTrue(*FString::Printf(TEXT("tile (%d,%d) blits back"), Col, Row),
                BlitInto(Rebuilt, TileBitmap, static_cast<int32>(Origin.X), static_cast<int32>(Origin.Y), Err));
        }
    }

    int32 Mismatches = 0;
    for (int32 Index = 0; Index < Mosaic.Pixels.Num(); ++Index)
    {
        if (Rebuilt.Pixels[Index] != Mosaic.Pixels[Index])
        {
            ++Mismatches;
        }
    }
    // Fails in both directions: a pixel that never got written (a gap between tiles) and a pixel
    // written from the wrong tile both land here.
    TestEqual(TEXT("cut + reassemble reproduces the mosaic exactly"), Mismatches, 0);
    return true;
}

// ============================================================================
// The slicer's seam agrees with TileGridUtils, recovered from the bytes
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwImageTileSeamRuleTest,
    "PinWright.image.tile.SeamRuleMatchesTileGrid",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FPwImageTileSeamRuleTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightImage;
    using namespace PinWrightTileGrid;

    constexpr int32 Width = 12;
    constexpr int32 Height = 6;
    const FBitmap Mosaic = PwImageTestMakeIdentityMosaic(Width, Height);

    FGeoreference Geo;
    FString ErrCode;
    FString ErrMsg;
    if (!ParseGeoreference(PwImageTestGeoreferenceJson(3, 2, -600.0, -300.0, 600.0, 300.0),
        Width, Height, false, Geo, ErrCode, ErrMsg))
    {
        AddError(FString::Printf(TEXT("ParseGeoreference failed: %s / %s"), *ErrCode, *ErrMsg));
        return false;
    }

    int32 PixelsChecked = 0;
    int32 SeamPixelsChecked = 0;
    for (int32 Row = 0; Row < Geo.Grid.Rows; ++Row)
    {
        for (int32 Col = 0; Col < Geo.Grid.Cols; ++Col)
        {
            FBitmap TileBitmap;
            FString Err;
            if (!ExtractTile(Mosaic, Geo.Grid, FTileIndex(Col, Row), TileBitmap, Err))
            {
                AddError(Err);
                return false;
            }

            for (int32 LocalY = 0; LocalY < TileBitmap.Height; ++LocalY)
            {
                for (int32 LocalX = 0; LocalX < TileBitmap.Width; ++LocalX)
                {
                    // Where the slicer ACTUALLY took this pixel from, read out of the bytes.
                    int32 MosaicX = INDEX_NONE;
                    int32 MosaicY = INDEX_NONE;
                    if (!TestTrue(TEXT("tile pixel carries a decodable mosaic identity"),
                        PwImageTestDecodeColor(TileBitmap.Pixels[LocalY * TileBitmap.Width + LocalX],
                            Width, MosaicX, MosaicY)))
                    {
                        return false;
                    }

                    // Where the georeference says that mosaic pixel belongs. This is the ONLY
                    // statement of the seam rule anywhere in the test.
                    FTilePixel Expected;
                    if (!TestTrue(TEXT("mosaic pixel is inside the image"),
                        PixelToTilePixel(Geo.Grid,
                            FVector2D(static_cast<double>(MosaicX) + 0.5,
                                static_cast<double>(MosaicY) + 0.5), Expected)))
                    {
                        return false;
                    }

                    TestEqual(*FString::Printf(TEXT("mosaic pixel (%d,%d) column"), MosaicX, MosaicY),
                        Expected.Tile.Col, Col);
                    TestEqual(*FString::Printf(TEXT("mosaic pixel (%d,%d) row"), MosaicX, MosaicY),
                        Expected.Tile.Row, Row);
                    TestEqual(*FString::Printf(TEXT("mosaic pixel (%d,%d) local X"), MosaicX, MosaicY),
                        FMath::FloorToInt32(Expected.PixelInTile.X), LocalX);
                    TestEqual(*FString::Printf(TEXT("mosaic pixel (%d,%d) local Y"), MosaicX, MosaicY),
                        FMath::FloorToInt32(Expected.PixelInTile.Y), LocalY);

                    ++PixelsChecked;
                    if (MosaicX % Geo.Grid.TilePixelWidth == 0 || MosaicY % Geo.Grid.TilePixelHeight == 0)
                    {
                        ++SeamPixelsChecked;
                    }
                }
            }
        }
    }

    // A slicer that dropped a column would still pass every assertion above on the pixels it did
    // emit, so assert the coverage too.
    TestEqual(TEXT("every mosaic pixel appeared in exactly one tile"), PixelsChecked, Width * Height);
    TestTrue(TEXT("seam-adjacent pixels were actually exercised"), SeamPixelsChecked > 0);

    // The far image edge has no tile to its right/below. TileGridUtils clamps it back into the
    // last tile at PixelInTile == the tile size - i.e. exactly one past the last pixel that tile
    // owns, which is what makes the half-open slice above complete.
    FTilePixel FarEdge;
    TestTrue(TEXT("the far image corner addresses a tile"),
        PixelToTilePixel(Geo.Grid,
            FVector2D(static_cast<double>(Geo.Grid.ImageWidth()), static_cast<double>(Geo.Grid.ImageHeight())),
            FarEdge));
    TestEqual(TEXT("far corner clamps into the last column"), FarEdge.Tile.Col, Geo.Grid.Cols - 1);
    TestEqual(TEXT("far corner clamps into the last row"), FarEdge.Tile.Row, Geo.Grid.Rows - 1);
    const FVector2D LastOrigin = TilePixelToPixel(Geo.Grid,
        FTileIndex(Geo.Grid.Cols - 1, Geo.Grid.Rows - 1), FVector2D::ZeroVector);
    TestTrue(TEXT("last tile's half-open end IS the far image corner"),
        (LastOrigin + FarEdge.PixelInTile).Equals(
            FVector2D(Geo.Grid.ImageWidth(), Geo.Grid.ImageHeight()), 1.0e-9));
    return true;
}

// ============================================================================
// A grid that does not divide the image is refused, not rounded
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwImageTileIndivisibleTest,
    "PinWright.image.tile.RejectsIndivisibleImage",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FPwImageTileIndivisibleTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightImage;

    FGeoreference Geo;
    FString ErrCode;
    FString ErrMsg;
    const bool bParsed = ParseGeoreference(PwImageTestGeoreferenceJson(5, 2, -600.0, -300.0, 600.0, 300.0),
        /*ImageWidth=*/12, /*ImageHeight=*/6, false, Geo, ErrCode, ErrMsg);
    TestFalse(TEXT("12 px does not divide into 5 tiles"), bParsed);
    TestEqual(TEXT("refusal is typed"), ErrCode, FString(TEXT("IMAGE_GRID_MISMATCH")));

    // The same image DOES divide into 3 x 2, so the refusal above is about the divisor and not
    // about the parser rejecting everything.
    FGeoreference Ok;
    TestTrue(TEXT("a divisible subdivision still parses"),
        ParseGeoreference(PwImageTestGeoreferenceJson(3, 2, -600.0, -300.0, 600.0, 300.0),
            12, 6, false, Ok, ErrCode, ErrMsg));
    return true;
}

// ============================================================================
// The georeference is resolution-free and round-trips
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwImageGeoreferenceRoundTripTest,
    "PinWright.image.tile.GeoreferenceRoundTripsAcrossResolutions",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FPwImageGeoreferenceRoundTripTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightImage;

    const TSharedPtr<FJsonObject> Json = PwImageTestGeoreferenceJson(4, 2, -10000.0, -5000.0, 10000.0, 5000.0);

    FGeoreference Small;
    FGeoreference Large;
    FString ErrCode;
    FString ErrMsg;
    TestTrue(TEXT("parses against a 512 x 256 image"),
        ParseGeoreference(Json, 512, 256, false, Small, ErrCode, ErrMsg));
    TestTrue(TEXT("parses against a 4096 x 2048 image"),
        ParseGeoreference(Json, 4096, 2048, false, Large, ErrCode, ErrMsg));

    // Different pixel scales, SAME ground - which is the whole reason the wire georeference
    // carries no pixel size.
    TestEqual(TEXT("small tile pixel width"), Small.Grid.TilePixelWidth, 128);
    TestEqual(TEXT("large tile pixel width"), Large.Grid.TilePixelWidth, 1024);
    FString Why;
    TestTrue(TEXT("the two describe the same ground"), GeoreferencesAgree(Small, Large, Why));
    if (!Why.IsEmpty())
    {
        AddError(Why);
    }

    // Re-parsing what SerializeGeoreference emits must produce the same georeference; the manifest
    // is only useful if the object it carries can be fed straight back in.
    FGeoreference Reparsed;
    TestTrue(TEXT("the serialized georeference parses back"),
        ParseGeoreference(SerializeGeoreference(Small), 512, 256, false, Reparsed, ErrCode, ErrMsg));
    TestTrue(TEXT("the serialized georeference is the same ground"),
        GeoreferencesAgree(Small, Reparsed, Why));

    // ---- depth: the one axis the round trip above cannot see ----
    //
    // Every georeference fixture in this file (and in TestImageHandlers.cpp, and in
    // TestOrthoTileCapture.cpp) is FLAT on the depth axis - z spans 0 to 0 - so DepthCm is always
    // 0 and bDepthExplicit is always false. Against such a fixture the round trip agrees whether
    // or not the serializer carries depth at all: delete SerializeGeoreference's `depthCm` field
    // and the reader's default (the midpoint of the AABB's depth span) hands back the same 0.
    // GeoreferencesAgree is blind to WorldMin/WorldMax as well - it compares Grid.World, which has
    // already dropped the depth axis - so only a depth-CARRYING fixture separates the two.
    {
        auto DepthPoint = [](double X, double Y, double Z)
        {
            TSharedPtr<FJsonObject> Point = MakeShared<FJsonObject>();
            Point->SetNumberField(TEXT("x"), X);
            Point->SetNumberField(TEXT("y"), Y);
            Point->SetNumberField(TEXT("z"), Z);
            return Point;
        };
        // top_down_x_right_y_down puts the depth axis on Z, so the AABB spans 100..500 cm of it.
        TSharedPtr<FJsonObject> Deep = PwImageTestGeoreferenceJson(1, 1, -100.0, -100.0, 100.0, 100.0);
        Deep->SetObjectField(TEXT("worldMin"), DepthPoint(-100.0, -100.0, 100.0));
        Deep->SetObjectField(TEXT("worldMax"), DepthPoint(100.0, 100.0, 500.0));

        FGeoreference Implicit;
        TestTrue(TEXT("a depth-carrying georeference parses"),
            ParseGeoreference(Deep, 64, 64, false, Implicit, ErrCode, ErrMsg));
        TestEqual(TEXT("an unstated depth is the midpoint of the AABB's depth span"),
            Implicit.DepthCm, 300.0, 1.0e-9);
        TestFalse(TEXT("an unstated depth is not reported as explicit"), Implicit.bDepthExplicit);

        // 412.5 is deliberately NOT the midpoint and not any other number this object carries, so
        // a reader that recomputed depth instead of reading it lands on 300 and fails here.
        Deep->SetNumberField(TEXT("depthCm"), 412.5);
        FGeoreference Explicit;
        TestTrue(TEXT("an explicit depth parses"),
            ParseGeoreference(Deep, 64, 64, false, Explicit, ErrCode, ErrMsg));
        TestEqual(TEXT("an explicit depth overrides the midpoint"), Explicit.DepthCm, 412.5, 1.0e-9);
        TestTrue(TEXT("an explicit depth is reported as explicit"), Explicit.bDepthExplicit);

        FGeoreference DeepReparsed;
        TestTrue(TEXT("the serialized depth-carrying georeference parses back"),
            ParseGeoreference(SerializeGeoreference(Explicit), 64, 64, false, DeepReparsed,
                ErrCode, ErrMsg));
        TestEqual(TEXT("depthCm survives the serialize/parse round trip"),
            DeepReparsed.DepthCm, 412.5, 1.0e-9);
        // The 3D AABB is kept verbatim rather than rebuilt from the in-plane extent (which has
        // dropped the depth axis). Nothing else asserts that: GeoreferencesAgree never looks.
        TestEqual(TEXT("worldMin survives verbatim, depth axis included"),
            DeepReparsed.WorldMin.Z, 100.0, 1.0e-9);
        TestEqual(TEXT("worldMax survives verbatim, depth axis included"),
            DeepReparsed.WorldMax.Z, 500.0, 1.0e-9);

        // Two georeferences differing ONLY in depth are different ground. The shifted-extent case
        // below returns on the extent check, so this is the only path that reaches the depth
        // comparison inside GeoreferencesAgree at all.
        TestFalse(TEXT("a different depth plane is refused as different ground"),
            GeoreferencesAgree(Implicit, Explicit, Why));
        TestTrue(TEXT("the refusal names the depth plane"), Why.Contains(TEXT("depth")));
    }

    // A different world box is NOT the same ground, and the disagreement is named.
    FGeoreference Moved;
    TestTrue(TEXT("shifted georeference parses"),
        ParseGeoreference(PwImageTestGeoreferenceJson(4, 2, -10000.0, -4000.0, 10000.0, 6000.0),
            512, 256, false, Moved, ErrCode, ErrMsg));
    TestFalse(TEXT("a shifted extent is refused as different ground"),
        GeoreferencesAgree(Small, Moved, Why));
    TestTrue(TEXT("the refusal names what differs"), Why.Contains(TEXT("extent")));

    // An unknown axes preset errors instead of falling back to a default mapping.
    TSharedPtr<FJsonObject> BadAxes = PwImageTestGeoreferenceJson(1, 1, -100.0, -100.0, 100.0, 100.0);
    BadAxes->SetStringField(TEXT("axes"), TEXT("isometric"));
    FGeoreference Unused;
    TestFalse(TEXT("an unknown axes preset is refused"),
        ParseGeoreference(BadAxes, 64, 64, false, Unused, ErrCode, ErrMsg));
    TestEqual(TEXT("refusal is typed"), ErrCode, FString(TEXT("INVALID_GEOREFERENCE")));

    // A typo in a nested key is rejected too - the dispatcher's UNKNOWN_PARAMS gate is top-level
    // only, so this is the only thing standing between a caller and a silently ignored field.
    TSharedPtr<FJsonObject> Typo = PwImageTestGeoreferenceJson(1, 1, -100.0, -100.0, 100.0, 100.0);
    Typo->SetNumberField(TEXT("depthcm"), 500.0);
    TestFalse(TEXT("an unknown georeference key is refused"),
        ParseGeoreference(Typo, 64, 64, false, Unused, ErrCode, ErrMsg));
    TestTrue(TEXT("the refusal names the offending key"), ErrMsg.Contains(TEXT("depthcm")));
    return true;
}

// ============================================================================
// A low-opacity overlay leaves the image underneath readable
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwImageAnnotateGridReadableTest,
    "PinWright.image.annotate.LowOpacityGridLeavesImageReadable",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FPwImageAnnotateGridReadableTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightImage;
    using namespace PinWrightBitmapPaint;

    // Two clearly distinguishable base tones in vertical bands, so "can the reader still tell them
    // apart THROUGH the overlay" is a measurable question rather than an impression.
    constexpr int32 Width = 32;
    constexpr int32 Height = 8;
    const FColor Dark(10, 10, 10, 255);
    const FColor Bright(200, 60, 60, 255);
    constexpr int32 Row = 4;

    auto MakeBanded = [&]() -> FBitmap
    {
        FBitmap Bitmap;
        FString Err;
        MakeBitmap(Width, Height, Dark, Bitmap, Err);
        for (int32 Y = 0; Y < Height; ++Y)
        {
            for (int32 X = 0; X < Width; ++X)
            {
                Bitmap.Pixels[Y * Width + X] = ((X / 4) % 2 == 0) ? Dark : Bright;
            }
        }
        return Bitmap;
    };

    const int32 DarkX = 1;    // inside a Dark band
    const int32 BrightX = 5;  // inside a Bright band
    const int32 OriginalContrast = FMath::Abs(static_cast<int32>(Bright.R) - static_cast<int32>(Dark.R));

    // --- low opacity: the line lands AND the bands survive it ---
    FBitmap Faint = MakeBanded();
    const FColor DarkBefore = Faint.Pixels[Row * Width + DarkX];
    {
        FSurface Surface = Faint.Surface();
        DrawLine(Surface, 0, Row, Width - 1, Row, 1, FPaint(FColor::White, 0.15f));
    }
    const FColor FaintDark = Faint.Pixels[Row * Width + DarkX];
    const FColor FaintBright = Faint.Pixels[Row * Width + BrightX];

    TestTrue(TEXT("the faint line actually changed the pixels it covered"), FaintDark != DarkBefore);

    const int32 FaintContrast =
        FMath::Abs(static_cast<int32>(FaintBright.R) - static_cast<int32>(FaintDark.R));
    // The real assertion: what is UNDER the overlay is still distinguishable, not merely that
    // something was drawn. A grid that buries its own subject passes "something was drawn".
    TestTrue(*FString::Printf(TEXT("bands stay distinguishable under the faint line (contrast %d of %d)"),
        FaintContrast, OriginalContrast), FaintContrast > 100);
    TestTrue(TEXT("the faint line costs less than a quarter of the original contrast"),
        FaintContrast > (OriginalContrast * 3) / 4);

    // Pixels OFF the line are untouched, so the overlay is not a global tint.
    TestEqual(TEXT("a row the line did not cross is byte-identical"),
        Faint.Pixels[(Row + 2) * Width + BrightX], Bright);

    // --- the failure direction: at full opacity the two bands become the same pixel ---
    FBitmap Opaque = MakeBanded();
    {
        FSurface Surface = Opaque.Surface();
        DrawLine(Surface, 0, Row, Width - 1, Row, 1, FPaint(FColor::White, 1.0f));
    }
    const int32 OpaqueContrast = FMath::Abs(
        static_cast<int32>(Opaque.Pixels[Row * Width + BrightX].R) -
        static_cast<int32>(Opaque.Pixels[Row * Width + DarkX].R));
    TestEqual(TEXT("a full-opacity line erases the distinction it was drawn to measure"),
        OpaqueContrast, 0);
    return true;
}
