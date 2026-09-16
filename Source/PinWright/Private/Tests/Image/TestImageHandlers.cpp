// Copyright (c) 2026 Alexander Penkin. MIT License.

// Handler-level tests for image.tile / image.annotate / image.compare.
//
// These drive the real registered handlers through InvokeHandlerWithCapture against synthetic PNGs
// in a per-run scratch directory, so they need no editor world, no viewport and no RHI - the three
// verbs touch only the filesystem and the CPU. Every test deletes its scratch tree on exit.
//
// The two that carry the design:
//
//  * RoundTripThroughWrittenTilesIsLossless cuts a mosaic with the real verb, reads the PNGs back
//    off disk and reassembles them at the pixel origins the MANIFEST reports. It therefore proves
//    the slice, the PNG round trip and the recorded origins agree - a defect in any one of the
//    three breaks it.
//  * TileAndFullImageAgree annotates the same WORLD coordinate onto the whole mosaic and onto one
//    extracted tile, then asserts the tile's rectangle of the annotated mosaic is byte-identical
//    to the annotated tile. That is the property the whole feature rests on: the tile view and the
//    mosaic view are the same georeference, not two conventions that happen to look similar.

#include "Misc/AutomationTest.h"

#include "Handlers/HandlerContext.h"
#include "Handlers/HandlerRegistration.h"
#include "Handlers/Image/ImageOps.h"
#include "Handlers/Render/TileGridUtils.h"
#include "Tests/TestUtils.h"
#include "Tests/Infra/DispatcherTestHelpers.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace
{
    FString PwImageHandlerMakeScratchDir()
    {
        const FString Dir = FPaths::ProjectSavedDir() / TEXT("PinWright") / TEXT("image") /
            TEXT("_test") / FGuid::NewGuid().ToString(EGuidFormats::Digits);
        IFileManager::Get().MakeDirectory(*Dir, true);
        return Dir;
    }

    FColor PwImageHandlerColorFor(int32 X, int32 Y, int32 Width)
    {
        const int32 Index = Y * Width + X;
        return FColor(
            static_cast<uint8>(Index & 0xFF),
            static_cast<uint8>((Index >> 8) & 0xFF),
            static_cast<uint8>(0x40 + ((Index >> 16) & 0x3F)),
            255);
    }

    PinWrightImage::FBitmap PwImageHandlerMakeMosaic(int32 Width, int32 Height)
    {
        PinWrightImage::FBitmap Bitmap;
        FString Err;
        PinWrightImage::MakeBitmap(Width, Height, FColor::Black, Bitmap, Err);
        for (int32 Y = 0; Y < Height; ++Y)
        {
            for (int32 X = 0; X < Width; ++X)
            {
                Bitmap.Pixels[Y * Width + X] = PwImageHandlerColorFor(X, Y, Width);
            }
        }
        return Bitmap;
    }

    TSharedPtr<FJsonObject> PwImageHandlerVec(double X, double Y, double Z)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetNumberField(TEXT("x"), X);
        Obj->SetNumberField(TEXT("y"), Y);
        Obj->SetNumberField(TEXT("z"), Z);
        return Obj;
    }

    // MinZ/MaxZ span the DEPTH axis (top_down_x_right_y_down puts it on Z). They default to a flat
    // 0..0 because most cases here do not care - but a flat depth makes DepthCm always 0, which is
    // also what the parser's default derivation produces, so a case that means to test depth has
    // to give it a span.
    TSharedPtr<FJsonObject> PwImageHandlerGeoreference(int32 Cols, int32 Rows,
        double MinX, double MinY, double MaxX, double MaxY, double MinZ = 0.0, double MaxZ = 0.0)
    {
        TSharedPtr<FJsonObject> Geo = MakeShared<FJsonObject>();
        Geo->SetStringField(TEXT("axes"), TEXT("top_down_x_right_y_down"));
        Geo->SetObjectField(TEXT("worldMin"), PwImageHandlerVec(MinX, MinY, MinZ));
        Geo->SetObjectField(TEXT("worldMax"), PwImageHandlerVec(MaxX, MaxY, MaxZ));
        Geo->SetNumberField(TEXT("cols"), Cols);
        Geo->SetNumberField(TEXT("rows"), Rows);
        return Geo;
    }

    bool PwImageHandlerLoadJson(const FString& Path, TSharedPtr<FJsonObject>& Out)
    {
        FString Text;
        if (!FFileHelper::LoadFileToString(Text, *Path))
        {
            return false;
        }
        const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
        return FJsonSerializer::Deserialize(Reader, Out) && Out.IsValid();
    }
}

// ============================================================================
// image.tile: tiles + manifest on disk, and the cut is lossless end to end
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwImageTileHandlerRoundTripTest,
    "PinWright.image.tile.RoundTripThroughWrittenTilesIsLossless",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FPwImageTileHandlerRoundTripTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightImage;
    using namespace PinWrightTileGrid;

    const FString Scratch = PwImageHandlerMakeScratchDir();
    ON_SCOPE_EXIT { IFileManager::Get().DeleteDirectory(*Scratch, false, true); };

    constexpr int32 Width = 12;
    constexpr int32 Height = 6;
    const FBitmap Mosaic = PwImageHandlerMakeMosaic(Width, Height);
    const FString SourcePath = Scratch / TEXT("src.png");
    FString ErrCode;
    FString ErrMsg;
    if (!TestTrue(TEXT("source PNG written"), SaveBitmapPng(SourcePath, Mosaic, ErrCode, ErrMsg)))
    {
        AddError(ErrMsg);
        return false;
    }

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("image"), SourcePath);
    // The depth axis spans 200..800 cm, so the derived depth plane is 500 - a number no other
    // field of this georeference carries. A flat 0..0 depth (the rest of this file's default)
    // cannot tell a carried depth from a dropped one, because both read back as 0.
    Payload->SetObjectField(TEXT("georeference"),
        PwImageHandlerGeoreference(3, 2, -600.0, -300.0, 600.0, 300.0, 200.0, 800.0));
    Payload->SetStringField(TEXT("outputDir"), Scratch / TEXT("out"));
    Payload->SetStringField(TEXT("namePrefix"), TEXT("ref"));

    FTestResponseCapture Capture;
    if (!TestTrue(TEXT("image.tile is registered"),
        InvokeHandlerWithCapture(TEXT("image.tile"), Payload, Capture)))
    {
        return false;
    }
    if (!TestTrue(TEXT("image.tile succeeded"), Capture.bSuccess))
    {
        AddError(FString::Printf(TEXT("%s: %s"), *Capture.ErrorCode, *Capture.Message));
        return false;
    }

    int32 TilesWritten = 0;
    Capture.Result->TryGetNumberField(TEXT("tilesWritten"), TilesWritten);
    TestEqual(TEXT("all six tiles written"), TilesWritten, 6);

    FString ManifestPath;
    TestTrue(TEXT("response names a manifest"),
        Capture.Result->TryGetStringField(TEXT("manifest"), ManifestPath));
    TestTrue(TEXT("manifest is on disk"), IFileManager::Get().FileExists(*ManifestPath));

    TSharedPtr<FJsonObject> Manifest;
    if (!TestTrue(TEXT("manifest parses"), PwImageHandlerLoadJson(ManifestPath, Manifest)))
    {
        return false;
    }
    const TArray<TSharedPtr<FJsonValue>>* Tiles = nullptr;
    if (!TestTrue(TEXT("manifest lists tiles"), Manifest->TryGetArrayField(TEXT("tiles"), Tiles)) || !Tiles)
    {
        return false;
    }
    TestEqual(TEXT("manifest lists six tiles"), Tiles->Num(), 6);

    // Rebuild the mosaic from the FILES, at the origins the MANIFEST recorded. This is the whole
    // contract in one assertion: slice + PNG round trip + recorded origins.
    FBitmap Rebuilt;
    FString MakeErr;
    TestTrue(TEXT("rebuild canvas allocates"), MakeBitmap(Width, Height, FColor::Black, Rebuilt, MakeErr));

    // The manifest's world extents must be the georeference's own answer, not a second derivation.
    FGeoreference Geo;
    if (!TestTrue(TEXT("georeference reparses from the manifest"),
        ParseGeoreference(Manifest->GetObjectField(TEXT("georeference")), Width, Height, false,
            Geo, ErrCode, ErrMsg)))
    {
        AddError(FString::Printf(TEXT("%s: %s"), *ErrCode, *ErrMsg));
        return false;
    }
    // The depth plane the manifest records is the midpoint of the payload's 200..800 cm depth
    // span. It is not implied by anything else in the manifest, so a serializer that dropped
    // depthCm, or a parser that stopped deriving it, lands somewhere other than 500 here.
    TestEqual(TEXT("the manifest records the derived depth plane"), Geo.DepthCm, 500.0, 1.0e-9);

    for (const TSharedPtr<FJsonValue>& Entry : *Tiles)
    {
        const TSharedPtr<FJsonObject>* TileObj = nullptr;
        if (!TestTrue(TEXT("tile entry is an object"), Entry.IsValid() && Entry->TryGetObject(TileObj)))
        {
            return false;
        }
        int32 Col = INDEX_NONE;
        int32 Row = INDEX_NONE;
        int32 OriginX = INDEX_NONE;
        int32 OriginY = INDEX_NONE;
        FString TilePath;
        (*TileObj)->TryGetNumberField(TEXT("col"), Col);
        (*TileObj)->TryGetNumberField(TEXT("row"), Row);
        (*TileObj)->TryGetNumberField(TEXT("pixelOriginX"), OriginX);
        (*TileObj)->TryGetNumberField(TEXT("pixelOriginY"), OriginY);
        (*TileObj)->TryGetStringField(TEXT("path"), TilePath);

        FBitmap TileBitmap;
        if (!TestTrue(*FString::Printf(TEXT("tile (%d,%d) PNG loads"), Col, Row),
            LoadBitmap(TilePath, TileBitmap, ErrCode, ErrMsg)))
        {
            AddError(ErrMsg);
            return false;
        }
        FString BlitErr;
        TestTrue(*FString::Printf(TEXT("tile (%d,%d) blits at its recorded origin"), Col, Row),
            BlitInto(Rebuilt, TileBitmap, OriginX, OriginY, BlitErr));

        // The per-tile world extent equals TileWorldExtent's answer for the same tile.
        FWorldExtent2D Expected;
        TestTrue(TEXT("tile has a world extent"),
            TileWorldExtent(Geo.Grid, FTileIndex(Col, Row), Expected));
        const TSharedPtr<FJsonObject>* MinObj = nullptr;
        const TSharedPtr<FJsonObject>* MaxObj = nullptr;
        if ((*TileObj)->TryGetObjectField(TEXT("worldMin"), MinObj) &&
            (*TileObj)->TryGetObjectField(TEXT("worldMax"), MaxObj))
        {
            TestEqual(*FString::Printf(TEXT("tile (%d,%d) worldMin.x"), Col, Row),
                (*MinObj)->GetNumberField(TEXT("x")), Expected.Min.X, 1.0e-6);
            TestEqual(*FString::Printf(TEXT("tile (%d,%d) worldMax.y"), Col, Row),
                (*MaxObj)->GetNumberField(TEXT("y")), Expected.Max.Y, 1.0e-6);
            // A tile's AABB is FLAT on the depth axis, sitting on the parent's depth plane rather
            // than inheriting the parent's depth SLAB. GeoreferencesAgree compares Grid.World,
            // which has already dropped the depth axis, so nothing downstream would ever notice a
            // tile that carried 200..800 forward instead.
            TestEqual(*FString::Printf(TEXT("tile (%d,%d) worldMin.z sits on the depth plane"), Col, Row),
                (*MinObj)->GetNumberField(TEXT("z")), Geo.DepthCm, 1.0e-6);
            TestEqual(*FString::Printf(TEXT("tile (%d,%d) worldMax.z sits on the depth plane"), Col, Row),
                (*MaxObj)->GetNumberField(TEXT("z")), Geo.DepthCm, 1.0e-6);
        }
        else
        {
            AddError(TEXT("tile entry has no worldMin/worldMax"));
        }

        // The per-tile georeference is standalone: a 1x1 grid over that tile's ground.
        const TSharedPtr<FJsonObject>* TileGeoObj = nullptr;
        if (TestTrue(TEXT("tile carries a standalone georeference"),
            (*TileObj)->TryGetObjectField(TEXT("georeference"), TileGeoObj)))
        {
            FGeoreference TileGeo;
            TestTrue(TEXT("the per-tile georeference parses against the tile image"),
                ParseGeoreference(*TileGeoObj, TileBitmap.Width, TileBitmap.Height, false,
                    TileGeo, ErrCode, ErrMsg));
            TestEqual(TEXT("per-tile georeference is 1x1"), TileGeo.Grid.Cols, 1);
            TestEqual(TEXT("per-tile world span matches the parent tile"),
                TileGeo.Grid.World.SpanAcross(), Expected.SpanAcross(), 1.0e-6);
            // Same depth plane as the parent: a tile handed to image.annotate or image.compare
            // has to be the same GROUND, and GeoreferencesAgree counts depth as part of that.
            TestEqual(TEXT("per-tile georeference keeps the parent's depth plane"),
                TileGeo.DepthCm, Geo.DepthCm, 1.0e-9);
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
    TestEqual(TEXT("tiles written to disk reassemble into the exact source mosaic"), Mismatches, 0);
    return true;
}

// ============================================================================
// image.tile refuses an indivisible cut rather than rounding it
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwImageTileHandlerRefusesIndivisibleTest,
    "PinWright.image.tile.RefusesIndivisibleCut",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FPwImageTileHandlerRefusesIndivisibleTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightImage;

    const FString Scratch = PwImageHandlerMakeScratchDir();
    ON_SCOPE_EXIT { IFileManager::Get().DeleteDirectory(*Scratch, false, true); };

    const FBitmap Mosaic = PwImageHandlerMakeMosaic(12, 6);
    const FString SourcePath = Scratch / TEXT("src.png");
    FString ErrCode;
    FString ErrMsg;
    SaveBitmapPng(SourcePath, Mosaic, ErrCode, ErrMsg);

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("image"), SourcePath);
    Payload->SetObjectField(TEXT("georeference"),
        PwImageHandlerGeoreference(5, 2, -600.0, -300.0, 600.0, 300.0));
    Payload->SetStringField(TEXT("outputDir"), Scratch / TEXT("out"));

    FTestResponseCapture Capture;
    TestTrue(TEXT("image.tile is registered"),
        InvokeHandlerWithCapture(TEXT("image.tile"), Payload, Capture));
    TestFalse(TEXT("an indivisible cut is refused, not rounded"), Capture.bSuccess);
    TestEqual(TEXT("refusal is typed"), Capture.ErrorCode, FString(TEXT("IMAGE_GRID_MISMATCH")));

    // And nothing was written: a refusal that had already emitted three tiles would leave a
    // half-cut directory behind that looks like a completed run.
    TArray<FString> Written;
    IFileManager::Get().FindFilesRecursive(Written, *(Scratch / TEXT("out")), TEXT("*.png"), true, false, false);
    TestEqual(TEXT("no tiles were written by the refused call"), Written.Num(), 0);
    return true;
}

// ============================================================================
// image.annotate: a world coordinate lands on the same ground either way
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwImageAnnotateParityTest,
    "PinWright.image.annotate.TileAndFullImageAgree",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FPwImageAnnotateParityTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightImage;
    using namespace PinWrightTileGrid;

    const FString Scratch = PwImageHandlerMakeScratchDir();
    ON_SCOPE_EXIT { IFileManager::Get().DeleteDirectory(*Scratch, false, true); };

    // 64 x 32 over 6400 x 3200 cm, cut 4 x 2 -> 16 x 16 px tiles at 100 cm/px on both axes.
    constexpr int32 Width = 64;
    constexpr int32 Height = 32;
    constexpr int32 TileCol = 1;
    constexpr int32 TileRow = 0;
    const FBitmap Mosaic = PwImageHandlerMakeMosaic(Width, Height);
    const FString SourcePath = Scratch / TEXT("src.png");
    FString ErrCode;
    FString ErrMsg;
    if (!TestTrue(TEXT("source PNG written"), SaveBitmapPng(SourcePath, Mosaic, ErrCode, ErrMsg)))
    {
        AddError(ErrMsg);
        return false;
    }

    auto MakeGeo = [] { return PwImageHandlerGeoreference(4, 2, -3200.0, -1600.0, 3200.0, 1600.0); };

    // --- cut, so the tile under test is the verb's own output rather than a hand-made crop ---
    TSharedPtr<FJsonObject> TilePayload = MakeShared<FJsonObject>();
    TilePayload->SetStringField(TEXT("image"), SourcePath);
    TilePayload->SetObjectField(TEXT("georeference"), MakeGeo());
    TilePayload->SetStringField(TEXT("outputDir"), Scratch / TEXT("tiles"));
    TilePayload->SetStringField(TEXT("namePrefix"), TEXT("ref"));
    FTestResponseCapture TileCapture;
    InvokeHandlerWithCapture(TEXT("image.tile"), TilePayload, TileCapture);
    if (!TestTrue(TEXT("image.tile succeeded"), TileCapture.bSuccess))
    {
        AddError(FString::Printf(TEXT("%s: %s"), *TileCapture.ErrorCode, *TileCapture.Message));
        return false;
    }
    const FString TilePath = Scratch / TEXT("tiles") / TEXT("ref_r0c1.png");
    if (!TestTrue(TEXT("tile (1,0) exists"), IFileManager::Get().FileExists(*TilePath)))
    {
        return false;
    }

    // The world point: the centre of tile (1,0), which is well inside it, so the crosshair is not
    // clipped in either view and byte equality is a fair test.
    FGeoreference Geo;
    if (!ParseGeoreference(MakeGeo(), Width, Height, false, Geo, ErrCode, ErrMsg))
    {
        AddError(FString::Printf(TEXT("%s: %s"), *ErrCode, *ErrMsg));
        return false;
    }
    FWorldExtent2D TileExtent;
    TestTrue(TEXT("tile has a world extent"),
        TileWorldExtent(Geo.Grid, FTileIndex(TileCol, TileRow), TileExtent));
    FVector Target = FVector::ZeroVector;
    SetAxisValue(Target, Geo.Grid.Axes.ScreenXAxis, TileExtent.Centre().X);
    SetAxisValue(Target, Geo.Grid.Axes.ScreenYAxis, TileExtent.Centre().Y);

    auto MakeCrosshairs = [&Target]()
    {
        TSharedPtr<FJsonObject> Cross = MakeShared<FJsonObject>();
        Cross->SetNumberField(TEXT("x"), Target.X);
        Cross->SetNumberField(TEXT("y"), Target.Y);
        Cross->SetNumberField(TEXT("z"), 0.0);
        Cross->SetStringField(TEXT("color"), TEXT("red"));
        Cross->SetNumberField(TEXT("sizePx"), 8);
        Cross->SetNumberField(TEXT("thicknessPx"), 1);
        TArray<TSharedPtr<FJsonValue>> Array;
        Array.Add(MakeShared<FJsonValueObject>(Cross));
        return Array;
    };

    // --- annotate the whole mosaic ---
    TSharedPtr<FJsonObject> FullPayload = MakeShared<FJsonObject>();
    FullPayload->SetStringField(TEXT("image"), SourcePath);
    FullPayload->SetObjectField(TEXT("georeference"), MakeGeo());
    FullPayload->SetArrayField(TEXT("crosshairs"), MakeCrosshairs());
    FullPayload->SetStringField(TEXT("outputDir"), Scratch / TEXT("annotated"));
    FullPayload->SetStringField(TEXT("outputName"), TEXT("full"));
    FTestResponseCapture FullCapture;
    InvokeHandlerWithCapture(TEXT("image.annotate"), FullPayload, FullCapture);
    if (!TestTrue(TEXT("annotating the full mosaic succeeded"), FullCapture.bSuccess))
    {
        AddError(FString::Printf(TEXT("%s: %s"), *FullCapture.ErrorCode, *FullCapture.Message));
        return false;
    }

    // --- annotate the single tile, same world point, same georeference ---
    TSharedPtr<FJsonObject> TileIndex = MakeShared<FJsonObject>();
    TileIndex->SetNumberField(TEXT("col"), TileCol);
    TileIndex->SetNumberField(TEXT("row"), TileRow);
    TSharedPtr<FJsonObject> TileAnnotatePayload = MakeShared<FJsonObject>();
    TileAnnotatePayload->SetStringField(TEXT("image"), TilePath);
    TileAnnotatePayload->SetObjectField(TEXT("georeference"), MakeGeo());
    TileAnnotatePayload->SetObjectField(TEXT("tile"), TileIndex);
    TileAnnotatePayload->SetArrayField(TEXT("crosshairs"), MakeCrosshairs());
    TileAnnotatePayload->SetStringField(TEXT("outputDir"), Scratch / TEXT("annotated"));
    TileAnnotatePayload->SetStringField(TEXT("outputName"), TEXT("tile"));
    FTestResponseCapture TileAnnotateCapture;
    InvokeHandlerWithCapture(TEXT("image.annotate"), TileAnnotatePayload, TileAnnotateCapture);
    if (!TestTrue(TEXT("annotating the tile succeeded"), TileAnnotateCapture.bSuccess))
    {
        AddError(FString::Printf(TEXT("%s: %s"), *TileAnnotateCapture.ErrorCode, *TileAnnotateCapture.Message));
        return false;
    }

    // --- the reported mosaic pixel is the same number from both views ---
    auto ReadMosaicPixel = [](const FTestResponseCapture& Capture, FVector2D& Out) -> bool
    {
        const TSharedPtr<FJsonObject>* Overlays = nullptr;
        if (!Capture.Result.IsValid() || !Capture.Result->TryGetObjectField(TEXT("overlays"), Overlays))
        {
            return false;
        }
        const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
        if (!(*Overlays)->TryGetArrayField(TEXT("crosshairs"), Rows) || !Rows || Rows->Num() != 1)
        {
            return false;
        }
        const TSharedPtr<FJsonObject>* Row = nullptr;
        if (!(*Rows)[0]->TryGetObject(Row))
        {
            return false;
        }
        const TSharedPtr<FJsonObject>* Pixel = nullptr;
        if (!(*Row)->TryGetObjectField(TEXT("mosaicPixel"), Pixel))
        {
            return false;
        }
        Out = FVector2D((*Pixel)->GetNumberField(TEXT("x")), (*Pixel)->GetNumberField(TEXT("y")));
        // A mark reported as landing somewhere must also have changed pixels there.
        bool bDrawn = false;
        (*Row)->TryGetBoolField(TEXT("drawn"), bDrawn);
        return bDrawn;
    };

    FVector2D FullPixel = FVector2D::ZeroVector;
    FVector2D TilePixel = FVector2D::ZeroVector;
    TestTrue(TEXT("full-image crosshair reports a pixel and was drawn"), ReadMosaicPixel(FullCapture, FullPixel));
    TestTrue(TEXT("tile crosshair reports a pixel and was drawn"), ReadMosaicPixel(TileAnnotateCapture, TilePixel));
    TestTrue(*FString::Printf(TEXT("both views agree on the mosaic pixel (%s vs %s)"),
        *FullPixel.ToString(), *TilePixel.ToString()), FullPixel.Equals(TilePixel, 1.0e-9));

    // --- and the pixels themselves agree ---
    FBitmap AnnotatedFull;
    FBitmap AnnotatedTile;
    if (!TestTrue(TEXT("annotated mosaic loads"),
            LoadBitmap(Scratch / TEXT("annotated") / TEXT("full.png"), AnnotatedFull, ErrCode, ErrMsg)) ||
        !TestTrue(TEXT("annotated tile loads"),
            LoadBitmap(Scratch / TEXT("annotated") / TEXT("tile.png"), AnnotatedTile, ErrCode, ErrMsg)))
    {
        AddError(ErrMsg);
        return false;
    }

    const FVector2D Origin = TilePixelToPixel(Geo.Grid, FTileIndex(TileCol, TileRow), FVector2D::ZeroVector);
    int32 RegionMismatches = 0;
    int32 MarkedPixels = 0;
    for (int32 Y = 0; Y < AnnotatedTile.Height; ++Y)
    {
        for (int32 X = 0; X < AnnotatedTile.Width; ++X)
        {
            const FColor FromTile = AnnotatedTile.Pixels[Y * AnnotatedTile.Width + X];
            const FColor FromMosaic = AnnotatedFull.Pixels[
                (static_cast<int32>(Origin.Y) + Y) * AnnotatedFull.Width + static_cast<int32>(Origin.X) + X];
            if (FromTile != FromMosaic)
            {
                ++RegionMismatches;
            }
            if (FromTile != Mosaic.Pixels[
                (static_cast<int32>(Origin.Y) + Y) * Width + static_cast<int32>(Origin.X) + X])
            {
                ++MarkedPixels;
            }
        }
    }
    TestEqual(TEXT("the tile region of the annotated mosaic is byte-identical to the annotated tile"),
        RegionMismatches, 0);
    // Guards the vacuous pass: if nothing had been drawn at all, the two would also match.
    TestTrue(*FString::Printf(TEXT("the crosshair actually marked pixels (%d changed)"), MarkedPixels),
        MarkedPixels > 0);
    return true;
}

// ============================================================================
// image.annotate refuses an empty mark set
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwImageAnnotateEmptyTest,
    "PinWright.image.annotate.RejectsEmptyMarkSet",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FPwImageAnnotateEmptyTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightImage;

    const FString Scratch = PwImageHandlerMakeScratchDir();
    ON_SCOPE_EXIT { IFileManager::Get().DeleteDirectory(*Scratch, false, true); };

    const FBitmap Mosaic = PwImageHandlerMakeMosaic(16, 16);
    const FString SourcePath = Scratch / TEXT("src.png");
    FString ErrCode;
    FString ErrMsg;
    SaveBitmapPng(SourcePath, Mosaic, ErrCode, ErrMsg);

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("image"), SourcePath);
    Payload->SetObjectField(TEXT("georeference"),
        PwImageHandlerGeoreference(1, 1, -800.0, -800.0, 800.0, 800.0));
    Payload->SetStringField(TEXT("outputDir"), Scratch / TEXT("out"));

    FTestResponseCapture Capture;
    TestTrue(TEXT("image.annotate is registered"),
        InvokeHandlerWithCapture(TEXT("image.annotate"), Payload, Capture));
    TestFalse(TEXT("an annotation with no marks is refused, not a copy"), Capture.bSuccess);
    TestEqual(TEXT("refusal is typed"), Capture.ErrorCode, FString(TEXT("NOTHING_TO_ANNOTATE")));

    TArray<FString> Written;
    IFileManager::Get().FindFilesRecursive(Written, *(Scratch / TEXT("out")), TEXT("*.png"), true, false, false);
    TestEqual(TEXT("no output was written"), Written.Num(), 0);

    // The failure direction: the same call WITH a mark succeeds, so the refusal above is about the
    // empty mark set and not about the payload being unusable.
    TSharedPtr<FJsonObject> Cross = MakeShared<FJsonObject>();
    Cross->SetNumberField(TEXT("x"), 0.0);
    Cross->SetNumberField(TEXT("y"), 0.0);
    TArray<TSharedPtr<FJsonValue>> Crosshairs;
    Crosshairs.Add(MakeShared<FJsonValueObject>(Cross));
    Payload->SetArrayField(TEXT("crosshairs"), Crosshairs);

    FTestResponseCapture Second;
    InvokeHandlerWithCapture(TEXT("image.annotate"), Payload, Second);
    TestTrue(TEXT("the same call with one crosshair succeeds"), Second.bSuccess);
    // ...and the success is an annotation on disk, not a bare bSuccess. The refusal above is
    // observed as "no PNG"; the control direction has to be observed the same way or the pair
    // only proves that one code path returns true and the other returns false.
    FString SecondOutput;
    if (Second.Result.IsValid())
    {
        Second.Result->TryGetStringField(TEXT("output"), SecondOutput);
    }
    TestTrue(TEXT("the accepted call wrote its annotated PNG"),
        IFileManager::Get().FileExists(*SecondOutput));
    return true;
}

// ============================================================================
// image.annotate rejects an unknown wire parameter through the real dispatcher
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwImageAnnotateUnknownParamTest,
    "PinWright.image.annotate.UnknownParamRejected",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FPwImageAnnotateUnknownParamTest::RunTest(const FString& Parameters)
{
    FRpcDispatcher Dispatcher;
    DispatcherTestHelpers::FSinkPtr Sink;
    DispatcherTestHelpers::MakeDispatcher(Sink, Dispatcher);

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("image"), TEXT("nonexistent.png"));
    // A plausible-looking misspelling of `crosshairs`. Without the dispatcher's gate the verb
    // would run with no marks and answer NOTHING_TO_ANNOTATE, which points the caller at the
    // wrong problem.
    Payload->SetArrayField(TEXT("crosshair"), TArray<TSharedPtr<FJsonValue>>());

    bool bSuccess = true;
    FString ErrorCode;
    DispatcherTestHelpers::Dispatch(Dispatcher, Sink, TEXT("image.annotate"), TEXT("t1"),
        Payload, bSuccess, ErrorCode);
    TestFalse(TEXT("an unknown parameter is rejected"), bSuccess);
    TestEqual(TEXT("rejection is UNKNOWN_PARAMS"), ErrorCode, FString(TEXT("UNKNOWN_PARAMS")));
    return true;
}

// ============================================================================
// image.annotate: a grid whose line count overflows int32 is REFUSED, not drawn
// ============================================================================
//
// The density guard used to narrow span/spacing to int32 BEFORE comparing against the ceiling. A
// very small spacingCm makes the quotient exceed INT32_MAX; the narrowing wrapped negative,
// `> MaxGridLinesPerAxis` was then false, and the verb answered SUCCESS with linesAcross: 0 - it
// wrote an annotated PNG carrying no grid and reported that as an annotation (rpc-design.md
// §1). Every assertion here is written to break if the verb goes back to answering success.

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwImageAnnotateGridOverflowTest,
    "PinWright.image.annotate.GridCeilingSurvivesInt32Overflow",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FPwImageAnnotateGridOverflowTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightImage;

    const FString Scratch = PwImageHandlerMakeScratchDir();
    ON_SCOPE_EXIT { IFileManager::Get().DeleteDirectory(*Scratch, false, true); };

    FString ErrCode;
    FString ErrMsg;
    const FString SourcePath = Scratch / TEXT("src.png");
    SaveBitmapPng(SourcePath, PwImageHandlerMakeMosaic(16, 16), ErrCode, ErrMsg);
    const FString OutDir = Scratch / TEXT("out");

    // A 1600 x 1600 cm frame, so the requested line count per axis is 1600 / spacingCm.
    auto Annotate = [&](double SpacingCm, const TCHAR* OutName, FTestResponseCapture& Capture) -> bool
    {
        TSharedPtr<FJsonObject> Grid = MakeShared<FJsonObject>();
        Grid->SetNumberField(TEXT("spacingCm"), SpacingCm);
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("image"), SourcePath);
        Payload->SetObjectField(TEXT("georeference"),
            PwImageHandlerGeoreference(1, 1, -800.0, -800.0, 800.0, 800.0));
        Payload->SetStringField(TEXT("outputDir"), OutDir);
        Payload->SetStringField(TEXT("outputName"), OutName);
        Payload->SetObjectField(TEXT("grid"), Grid);
        return InvokeHandlerWithCapture(TEXT("image.annotate"), Payload, Capture);
    };

    // --- 1e-9 cm spacing: 1.6e12 lines per axis, which does not fit an int32 ---
    {
        FTestResponseCapture Capture;
        TestTrue(TEXT("image.annotate is registered"), Annotate(1.0e-9, TEXT("overflow"), Capture));
        TestFalse(TEXT("a line count past INT32_MAX is refused, not silently drawn as zero lines"),
            Capture.bSuccess);
        TestEqual(TEXT("refusal is typed"), Capture.ErrorCode, FString(TEXT("GRID_TOO_DENSE")));
        // The count no longer fits an int32, so the message must not print the wrapped value the
        // old narrowing produced, and must still name the ceiling that was broken.
        TestFalse(TEXT("the message does not print a wrapped negative count"),
            Capture.Message.Contains(TEXT("-2147483")));
        TestTrue(TEXT("the message names the per-axis ceiling"), Capture.Message.Contains(TEXT("512")));
    }

    // --- the same failure one step further: the count is not even finite ---
    {
        FTestResponseCapture Capture;
        Annotate(TNumericLimits<double>::Min(), TEXT("infinite"), Capture);
        TestFalse(TEXT("a non-finite line count is refused"), Capture.bSuccess);
        TestEqual(TEXT("refusal is typed"), Capture.ErrorCode, FString(TEXT("GRID_TOO_DENSE")));
    }

    // --- an ordinary too-dense spacing, which the pre-fix guard already caught ---
    {
        FTestResponseCapture Capture;
        Annotate(1.0, TEXT("dense"), Capture);
        TestFalse(TEXT("1601 lines per axis is refused"), Capture.bSuccess);
        TestEqual(TEXT("refusal is typed"), Capture.ErrorCode, FString(TEXT("GRID_TOO_DENSE")));
    }

    // The defect wrote an output file for the overflow case and called it an annotation, so the
    // absence of any PNG is the observation that separates a refusal from that silent no-op.
    TArray<FString> Written;
    IFileManager::Get().FindFilesRecursive(Written, *OutDir, TEXT("*.png"), true, false, false);
    TestEqual(TEXT("no refused call wrote an output"), Written.Num(), 0);

    // The other direction: a spacing INSIDE the ceiling still draws, so the refusals above are
    // about density and not about the payload being unusable.
    {
        FTestResponseCapture Capture;
        Annotate(200.0, TEXT("accepted"), Capture);
        if (!TestTrue(TEXT("a spacing inside the ceiling succeeds"), Capture.bSuccess))
        {
            AddError(FString::Printf(TEXT("%s: %s"), *Capture.ErrorCode, *Capture.Message));
            return false;
        }
        double LinesAcross = 0.0;
        double LinesDown = 0.0;
        double Opacity = 0.0;
        bool bGridDrawn = false;
        const TSharedPtr<FJsonObject>* Overlays = nullptr;
        const TSharedPtr<FJsonObject>* GridEcho = nullptr;
        if (TestTrue(TEXT("overlays reported"), Capture.Result.IsValid() &&
                Capture.Result->TryGetObjectField(TEXT("overlays"), Overlays)) &&
            TestTrue(TEXT("the grid overlay is reported"),
                (*Overlays)->TryGetObjectField(TEXT("grid"), GridEcho)))
        {
            (*GridEcho)->TryGetNumberField(TEXT("linesAcross"), LinesAcross);
            (*GridEcho)->TryGetNumberField(TEXT("linesDown"), LinesDown);
            (*GridEcho)->TryGetNumberField(TEXT("opacity"), Opacity);
            (*GridEcho)->TryGetBoolField(TEXT("drawn"), bGridDrawn);
        }
        // The count is knowable, so it is asserted rather than tested for `> 0`. A 1600 cm frame
        // at 200 cm spacing has 9 candidate lines per axis (-800, -600, ... 800); the last lands
        // on pixel 16 of a 16 px canvas - one past the last column - and is skipped, leaving 8.
        // `> 0` would survive an off-by-one that moved the whole grid by half a cell.
        TestEqual(TEXT("every across line inside the frame is drawn"),
            static_cast<int32>(LinesAcross), 8);
        TestEqual(TEXT("every down line inside the frame is drawn"),
            static_cast<int32>(LinesDown), 8);
        // linesAcross/linesDown count DrawLine CALLS - they are the request echoed back, and a
        // gutted painter still produces them. `drawn` is hashed off the canvas before and after,
        // so it is the only field here that reports what actually reached the pixels.
        TestTrue(TEXT("the accepted grid actually changed pixels"), bGridDrawn);
        // The documented default: a full-opacity grid buries the detail it was drawn to measure
        // (see the header note in Render/BitmapPaint.h). Nothing else pins this number - the
        // low-opacity unit test in TestImageOps.cpp passes its own 0.15 rather than the default.
        TestEqual(TEXT("the grid default opacity is the documented 0.25"), Opacity, 0.25, 1.0e-9);
        // And a success that wrote no file would be the silent no-op this whole test refuses.
        FString AcceptedOutput;
        if (Capture.Result.IsValid())
        {
            Capture.Result->TryGetStringField(TEXT("output"), AcceptedOutput);
        }
        TestTrue(TEXT("the accepted call wrote its annotated PNG"),
            IFileManager::Get().FileExists(*AcceptedOutput));
    }
    return true;
}

// ============================================================================
// image.compare: refuses incomparable inputs, reports comparable ones
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwImageCompareMismatchTest,
    "PinWright.image.compare.RejectsMismatchedInputs",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FPwImageCompareMismatchTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightImage;

    const FString Scratch = PwImageHandlerMakeScratchDir();
    ON_SCOPE_EXIT { IFileManager::Get().DeleteDirectory(*Scratch, false, true); };

    FString ErrCode;
    FString ErrMsg;
    const FString PathA = Scratch / TEXT("a.png");
    const FString PathB = Scratch / TEXT("b.png");
    const FString PathSmall = Scratch / TEXT("small.png");
    SaveBitmapPng(PathA, PwImageHandlerMakeMosaic(16, 16), ErrCode, ErrMsg);
    SaveBitmapPng(PathB, PwImageHandlerMakeMosaic(16, 16), ErrCode, ErrMsg);
    SaveBitmapPng(PathSmall, PwImageHandlerMakeMosaic(8, 8), ErrCode, ErrMsg);

    // --- different pixel dimensions ---
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("imageA"), PathA);
        Payload->SetStringField(TEXT("imageB"), PathSmall);
        Payload->SetStringField(TEXT("outputDir"), Scratch / TEXT("cmp"));
        FTestResponseCapture Capture;
        TestTrue(TEXT("image.compare is registered"),
            InvokeHandlerWithCapture(TEXT("image.compare"), Payload, Capture));
        TestFalse(TEXT("mismatched dimensions are refused"), Capture.bSuccess);
        TestEqual(TEXT("refusal is typed"), Capture.ErrorCode, FString(TEXT("IMAGE_SIZE_MISMATCH")));
    }

    // --- same pixels, different ground ---
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("imageA"), PathA);
        Payload->SetStringField(TEXT("imageB"), PathB);
        Payload->SetObjectField(TEXT("georeferenceA"),
            PwImageHandlerGeoreference(1, 1, -800.0, -800.0, 800.0, 800.0));
        Payload->SetObjectField(TEXT("georeferenceB"),
            PwImageHandlerGeoreference(1, 1, 0.0, 0.0, 1600.0, 1600.0));
        Payload->SetStringField(TEXT("outputDir"), Scratch / TEXT("cmp"));
        FTestResponseCapture Capture;
        InvokeHandlerWithCapture(TEXT("image.compare"), Payload, Capture);
        TestFalse(TEXT("mismatched georeferences are refused"), Capture.bSuccess);
        TestEqual(TEXT("refusal is typed"), Capture.ErrorCode, FString(TEXT("GEOREFERENCE_MISMATCH")));
    }

    // --- a one-sided georeference is refused rather than half-checked ---
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("imageA"), PathA);
        Payload->SetStringField(TEXT("imageB"), PathB);
        Payload->SetObjectField(TEXT("georeferenceA"),
            PwImageHandlerGeoreference(1, 1, -800.0, -800.0, 800.0, 800.0));
        Payload->SetStringField(TEXT("outputDir"), Scratch / TEXT("cmp"));
        FTestResponseCapture Capture;
        InvokeHandlerWithCapture(TEXT("image.compare"), Payload, Capture);
        TestFalse(TEXT("a one-sided georeference is refused"), Capture.bSuccess);
        TestEqual(TEXT("refusal is typed"), Capture.ErrorCode, FString(TEXT("INVALID_GEOREFERENCE")));
    }

    TArray<FString> Written;
    IFileManager::Get().FindFilesRecursive(Written, *(Scratch / TEXT("cmp")), TEXT("*.png"), true, false, false);
    TestEqual(TEXT("no composite was written by any refused call"), Written.Num(), 0);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwImageCompareMeasuresTest,
    "PinWright.image.compare.MeasuresDifferenceBothDirections",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FPwImageCompareMeasuresTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightImage;

    const FString Scratch = PwImageHandlerMakeScratchDir();
    ON_SCOPE_EXIT { IFileManager::Get().DeleteDirectory(*Scratch, false, true); };

    FString ErrCode;
    FString ErrMsg;
    FBitmap Base = PwImageHandlerMakeMosaic(16, 16);
    const FString PathA = Scratch / TEXT("a.png");
    const FString PathSame = Scratch / TEXT("same.png");
    SaveBitmapPng(PathA, Base, ErrCode, ErrMsg);
    SaveBitmapPng(PathSame, Base, ErrCode, ErrMsg);

    // One pixel differs by a known amount, so both the count and the magnitude are checkable.
    FBitmap Altered = Base;
    const int32 AlteredIndex = 5 * 16 + 7;
    const FColor Before = Altered.Pixels[AlteredIndex];
    Altered.Pixels[AlteredIndex] = FColor(
        static_cast<uint8>(FMath::Min(255, Before.R + 40)), Before.G, Before.B, 255);
    const FString PathAltered = Scratch / TEXT("altered.png");
    SaveBitmapPng(PathAltered, Altered, ErrCode, ErrMsg);

    auto ReadStats = [](const FTestResponseCapture& Capture, int64& OutDiffering, bool& OutIdentical,
        int32& OutMaxR) -> bool
    {
        const TSharedPtr<FJsonObject>* Stats = nullptr;
        if (!Capture.Result.IsValid() || !Capture.Result->TryGetObjectField(TEXT("stats"), Stats))
        {
            return false;
        }
        double Differing = 0.0;
        (*Stats)->TryGetNumberField(TEXT("differingPixels"), Differing);
        OutDiffering = static_cast<int64>(Differing);
        (*Stats)->TryGetBoolField(TEXT("identical"), OutIdentical);
        const TSharedPtr<FJsonObject>* MaxObj = nullptr;
        if ((*Stats)->TryGetObjectField(TEXT("maxAbsDifference"), MaxObj))
        {
            OutMaxR = static_cast<int32>((*MaxObj)->GetNumberField(TEXT("r")));
        }
        return true;
    };

    // --- identical inputs ---
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("imageA"), PathA);
        Payload->SetStringField(TEXT("imageB"), PathSame);
        Payload->SetStringField(TEXT("outputDir"), Scratch / TEXT("cmp"));
        Payload->SetStringField(TEXT("outputPrefix"), TEXT("same"));
        // Matching georeferences on both sides, because every OTHER georeference case in this
        // file is a refusal: a GeoreferencesAgree that answered false for everything would pass
        // all of them. This is the only call that takes the accepting branch of the gate.
        Payload->SetObjectField(TEXT("georeferenceA"),
            PwImageHandlerGeoreference(1, 1, -800.0, -800.0, 800.0, 800.0));
        Payload->SetObjectField(TEXT("georeferenceB"),
            PwImageHandlerGeoreference(1, 1, -800.0, -800.0, 800.0, 800.0));
        FTestResponseCapture Capture;
        InvokeHandlerWithCapture(TEXT("image.compare"), Payload, Capture);
        if (!TestTrue(TEXT("comparing identical images succeeds"), Capture.bSuccess))
        {
            AddError(FString::Printf(TEXT("%s: %s"), *Capture.ErrorCode, *Capture.Message));
            return false;
        }
        int64 Differing = -1;
        bool bIdentical = false;
        int32 MaxR = -1;
        TestTrue(TEXT("stats present"), ReadStats(Capture, Differing, bIdentical, MaxR));
        TestEqual(TEXT("identical images differ in zero pixels"), Differing, static_cast<int64>(0));
        TestTrue(TEXT("identical is reported"), bIdentical);
        TestEqual(TEXT("max channel difference is zero"), MaxR, 0);

        FString SideBySide;
        FString Difference;
        Capture.Result->TryGetStringField(TEXT("sideBySide"), SideBySide);
        Capture.Result->TryGetStringField(TEXT("difference"), Difference);
        TestTrue(TEXT("side-by-side written"), IFileManager::Get().FileExists(*SideBySide));
        TestTrue(TEXT("difference written"), IFileManager::Get().FileExists(*Difference));

        // The side-by-side is wide enough to hold both frames and the divider.
        FBitmap Loaded;
        TestTrue(TEXT("side-by-side loads"), LoadBitmap(SideBySide, Loaded, ErrCode, ErrMsg));
        TestEqual(TEXT("side-by-side width is A + gap + B"), Loaded.Width, 16 + 8 + 16);
        TestEqual(TEXT("side-by-side height matches the inputs"), Loaded.Height, 16);

        // The pair WAS measured against its ground rather than refused, and the accepted
        // georeference is echoed back at the images' own resolution.
        bool bGeoreferenced = false;
        Capture.Result->TryGetBoolField(TEXT("georeferenced"), bGeoreferenced);
        TestTrue(TEXT("a matching georeferenced pair is compared, not refused"), bGeoreferenced);
        const TSharedPtr<FJsonObject>* GridEcho = nullptr;
        if (TestTrue(TEXT("the accepted comparison echoes its grid"),
            Capture.Result->TryGetObjectField(TEXT("grid"), GridEcho)))
        {
            TestEqual(TEXT("the echoed grid is derived from the images"),
                static_cast<int32>((*GridEcho)->GetNumberField(TEXT("imageWidth"))), 16);
        }
    }

    // --- one differing pixel: the other failure direction ---
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("imageA"), PathA);
        Payload->SetStringField(TEXT("imageB"), PathAltered);
        Payload->SetStringField(TEXT("outputDir"), Scratch / TEXT("cmp"));
        Payload->SetStringField(TEXT("outputPrefix"), TEXT("altered"));
        FTestResponseCapture Capture;
        InvokeHandlerWithCapture(TEXT("image.compare"), Payload, Capture);
        if (!TestTrue(TEXT("comparing altered images succeeds"), Capture.bSuccess))
        {
            AddError(FString::Printf(TEXT("%s: %s"), *Capture.ErrorCode, *Capture.Message));
            return false;
        }
        int64 Differing = -1;
        bool bIdentical = true;
        int32 MaxR = -1;
        TestTrue(TEXT("stats present"), ReadStats(Capture, Differing, bIdentical, MaxR));
        TestEqual(TEXT("exactly one pixel differs"), Differing, static_cast<int64>(1));
        TestFalse(TEXT("identical is false when a pixel differs"), bIdentical);
        TestEqual(TEXT("the measured magnitude is the injected one"), MaxR, 40);
    }

    // --- the composites themselves, which none of the statistics above can see ---
    //
    // differingPixels / maxAbsDifference are accumulated into their own variables in the same
    // loop that fills the difference bitmap, so they are a proxy for the picture, not the picture:
    // write a constant into Difference.Pixels and every assertion above stays green. These read
    // the two PNGs back off disk instead. `amplify` (x4 here) is the parameter documented as
    // "it changes the picture, never the statistics", and nothing else exercises it at all;
    // `labels:false` keeps both halves of the side-by-side pure composite so they can be compared
    // byte for byte against their sources.
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("imageA"), PathA);
        Payload->SetStringField(TEXT("imageB"), PathAltered);
        Payload->SetStringField(TEXT("outputDir"), Scratch / TEXT("cmp"));
        Payload->SetStringField(TEXT("outputPrefix"), TEXT("detail"));
        Payload->SetNumberField(TEXT("amplify"), 4.0);
        Payload->SetBoolField(TEXT("labels"), false);
        FTestResponseCapture Capture;
        InvokeHandlerWithCapture(TEXT("image.compare"), Payload, Capture);
        if (!TestTrue(TEXT("the amplified comparison succeeds"), Capture.bSuccess))
        {
            AddError(FString::Printf(TEXT("%s: %s"), *Capture.ErrorCode, *Capture.Message));
            return false;
        }

        int64 Differing = -1;
        bool bIdentical = true;
        int32 MaxR = -1;
        TestTrue(TEXT("stats present"), ReadStats(Capture, Differing, bIdentical, MaxR));
        TestEqual(TEXT("amplify does not move the differing-pixel count"),
            Differing, static_cast<int64>(1));
        TestEqual(TEXT("amplify does not move the measured magnitude"), MaxR, 40);

        FString DifferencePath;
        FString SideBySidePath;
        Capture.Result->TryGetStringField(TEXT("difference"), DifferencePath);
        Capture.Result->TryGetStringField(TEXT("sideBySide"), SideBySidePath);

        FBitmap Diff;
        if (TestTrue(TEXT("the difference composite loads"),
            LoadBitmap(DifferencePath, Diff, ErrCode, ErrMsg)))
        {
            // The injected delta is 40 in R only, so at amplify 4 the composite must read 160
            // there - a value neither input carries, so no copy or constant produces it.
            TestEqual(TEXT("the difference composite paints the amplified delta"),
                Diff.Pixels[AlteredIndex], FColor(160, 0, 0, 255));
            int32 TransparentPixels = 0;
            int32 PaintedPixels = 0;
            for (const FColor& Pixel : Diff.Pixels)
            {
                if (Pixel.A != 255) { ++TransparentPixels; }
                if (Pixel.R != 0 || Pixel.G != 0 || Pixel.B != 0) { ++PaintedPixels; }
            }
            // Alpha 255 unconditionally is a stated invariant of this verb. An alpha-0 composite
            // keeps intact RGB and reads as blank to every viewer - the blank-capture failure in
            // another costume - and no statistic in the response would move.
            TestEqual(TEXT("no pixel of the difference composite is transparent"),
                TransparentPixels, 0);
            TestEqual(TEXT("exactly the one differing pixel is painted"), PaintedPixels, 1);
        }

        FBitmap Side;
        if (TestTrue(TEXT("the side-by-side loads"),
            LoadBitmap(SideBySidePath, Side, ErrCode, ErrMsg)))
        {
            // WHICH half holds which input. With only the width asserted, blitting A into both
            // halves - or swapping A and B - reads as a pass.
            int32 LeftMismatches = 0;
            int32 RightMismatches = 0;
            int32 GapMismatches = 0;
            for (int32 Y = 0; Y < 16; ++Y)
            {
                for (int32 X = 0; X < 16; ++X)
                {
                    if (Side.Pixels[Y * Side.Width + X] != Base.Pixels[Y * 16 + X])
                    {
                        ++LeftMismatches;
                    }
                    if (Side.Pixels[Y * Side.Width + 16 + 8 + X] != Altered.Pixels[Y * 16 + X])
                    {
                        ++RightMismatches;
                    }
                }
                for (int32 X = 16; X < 16 + 8; ++X)
                {
                    if (Side.Pixels[Y * Side.Width + X] != FColor(24, 24, 24, 255))
                    {
                        ++GapMismatches;
                    }
                }
            }
            TestEqual(TEXT("the left half is imageA"), LeftMismatches, 0);
            TestEqual(TEXT("the right half is imageB"), RightMismatches, 0);
            TestEqual(TEXT("the divider separates the two halves"), GapMismatches, 0);
        }
    }
    return true;
}
