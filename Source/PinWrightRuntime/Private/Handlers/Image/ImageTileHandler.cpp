// Copyright (c) 2026 Alexander Penkin. MIT License.

// image.tile - cut an image file into a georeferenced N x M grid, writing every tile plus a
// manifest that records, per tile, its world extent and its pixel origin.
//
// The verb owns no maths. Every world <-> pixel answer comes from PinWrightTileGrid and every
// slice comes from PinWrightImage::ExtractTile, which derives its origin from the same
// TilePixelToPixel the georeference uses - so the mosaic and the georeference cannot disagree
// about where a seam is.
//
// The manifest carries a per-tile `georeference` object that is itself a complete, standalone,
// RESOLUTION-FREE ground definition. That is what closes the loop the feature exists for: hand
// tile (r,c)'s georeference to image.annotate or image.compare and reference tile (r,c) and
// capture tile (r,c) are provably the same ground, whatever pixel counts the two images have.

#include "Handlers/HandlerContext.h"
#include "Handlers/HandlerRegistration.h"
#include "Handlers/ParamSpec.h"
#include "Handlers/ErrorCodes.h"
#include "Handlers/Image/ImageOps.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

// No file-scope `using namespace` anywhere in this file: Unity merges translation units, so a
// using-directive outside a function body leaks into every .cpp compiled after it in the same
// blob. The handler bodies below take theirs at function scope, which does not.
namespace
{
    // Above this the per-tile array is written to the manifest only. A 1024-tile array inline is
    // ~400 KB of response for data the caller already has on disk; the response says which
    // happened rather than silently shortening the list.
    constexpr int32 PwTileMaxTilesInResponse = 64;

    FString PwTilePad(int32 Value, int32 Digits)
    {
        FString Text = FString::FromInt(Value);
        while (Text.Len() < Digits)
        {
            Text = TEXT("0") + Text;
        }
        return Text;
    }

    FString PwTileFileName(const FString& Prefix, int32 Col, int32 Row, int32 ColDigits, int32 RowDigits)
    {
        return FString::Printf(TEXT("%s_r%sc%s.png"), *Prefix,
            *PwTilePad(Row, RowDigits), *PwTilePad(Col, ColDigits));
    }

    // The georeference of ONE tile: the same axes and depth, the tile's own world AABB, and a 1x1
    // subdivision. Built from TileWorldExtent so it is the georeference's own answer, not a second
    // derivation of it.
    bool PwTileMakeTileGeoreference(const PinWrightImage::FGeoreference& Parent,
        const PinWrightTileGrid::FTileIndex& Tile,
        FVector& OutWorldMin, FVector& OutWorldMax, TSharedPtr<FJsonObject>& OutGeo)
    {
        PinWrightTileGrid::FWorldExtent2D Extent;
        if (!PinWrightTileGrid::TileWorldExtent(Parent.Grid, Tile, Extent))
        {
            return false;
        }

        const PinWrightTileGrid::FScreenAxisMapping& Axes = Parent.Grid.Axes;
        FVector Min = FVector::ZeroVector;
        FVector Max = FVector::ZeroVector;
        PinWrightTileGrid::SetAxisValue(Min, Axes.ScreenXAxis, Extent.Min.X);
        PinWrightTileGrid::SetAxisValue(Min, Axes.ScreenYAxis, Extent.Min.Y);
        PinWrightTileGrid::SetAxisValue(Min, Axes.DepthAxis(), Parent.DepthCm);
        PinWrightTileGrid::SetAxisValue(Max, Axes.ScreenXAxis, Extent.Max.X);
        PinWrightTileGrid::SetAxisValue(Max, Axes.ScreenYAxis, Extent.Max.Y);
        PinWrightTileGrid::SetAxisValue(Max, Axes.DepthAxis(), Parent.DepthCm);

        OutWorldMin = Min;
        OutWorldMax = Max;

        OutGeo = MakeShared<FJsonObject>();
        OutGeo->SetField(TEXT("axes"), PinWrightImage::SerializeAxes(Axes));
        OutGeo->SetObjectField(TEXT("worldMin"), PinWrightImage::MakeVectorObject(Min));
        OutGeo->SetObjectField(TEXT("worldMax"), PinWrightImage::MakeVectorObject(Max));
        OutGeo->SetNumberField(TEXT("cols"), 1);
        OutGeo->SetNumberField(TEXT("rows"), 1);
        OutGeo->SetNumberField(TEXT("depthCm"), Parent.DepthCm);
        return true;
    }
}

REGISTER_RPC_HANDLER("image.tile", "image",
    "Cut an image file into a georeferenced N x M tile grid, writing every tile plus a manifest recording each tile's world extent, pixel origin and standalone georeference.",
    RPC_PARAMS(
        RPC_PARAM_REQ("image", "filepath",
            "Path to the source image. Absolute, or relative to the project directory. Any format FImageUtils decodes (PNG, JPG, EXR, ...); tiles are always written as PNG."),
        RPC_PARAM_OPT("georeference", "object",
            "The ground this image covers: {axes, worldMin, worldMax, cols, rows, depthCm?}. `axes` is a preset name (top_down_x_up_y_right | top_down_x_right_y_down | front_y_right_z_up | side_x_right_z_up) or {screenXAxis, screenXPositive, screenYAxis, screenYPositive}. worldMin/worldMax are world-space {x,y,z} corners in centimetres. cols/rows are required - there is no default subdivision. depthCm defaults to the midpoint of the AABB's depth-axis span. Carries NO pixel size: tile pixel size is always derived from this image, which is what lets one georeference describe two resolutions. Exactly one of georeference / georeferenceFrom."),
        RPC_PARAM_OPT("georeferenceFrom", "filepath",
            "Path to an image.tile manifest (or a bare georeference JSON file) whose georeference this call adopts verbatim. Exactly one of georeference / georeferenceFrom."),
        RPC_PARAM_OPT("georeferenceFromTile", "object",
            "{col, row} - take that tile's standalone georeference out of the manifest named by georeferenceFrom, instead of the whole-mosaic one. Use it to sub-tile a single tile."),
        RPC_PARAM_OPT("outputDir", "filepath",
            "Directory to write the tiles and manifest into. Defaults to <ProjectSaved>/PinWright/image/tiles/<namePrefix>."),
        RPC_PARAM_OPT("namePrefix", "string",
            "Filename stem for the tiles and manifest. Defaults to the source image's base name. Tiles are named <prefix>_r<row>c<col>.png, zero-padded to the grid's digit count."),
        RPC_PARAM_DEF("overwrite", "boolean",
            "Replace tiles/manifest that already exist at the target paths. When false (the default) the call refuses before writing anything, so a half-overwritten tile set is unreachable.",
            "false")
    ))
{
    using namespace PinWrightImage;
    using namespace PinWrightTileGrid;

    FString RequestedImage;
    if (!Ctx.RequireString(TEXT("image"), RequestedImage))
    {
        return true;
    }

    const FString SourcePath = ResolveInputPath(RequestedImage);
    FBitmap Source;
    FString ErrCode;
    FString ErrMsg;
    if (!LoadBitmap(SourcePath, Source, ErrCode, ErrMsg))
    {
        Ctx.SendError(ErrCode, ErrMsg);
        return true;
    }

    // ---- georeference ----
    FTileIndex FromTile;
    if (const TSharedPtr<FJsonObject> FromTileObj = Ctx.GetObject(TEXT("georeferenceFromTile")))
    {
        const TArray<FString> Allowed = { TEXT("col"), TEXT("row") };
        FString KeyErr;
        if (!RejectUnknownKeys(FromTileObj, Allowed, TEXT("georeferenceFromTile"), KeyErr))
        {
            Ctx.SendError(ErrorCodes::ERR_INVALID_PARAMS, KeyErr);
            return true;
        }
        int32 Col = INDEX_NONE;
        int32 Row = INDEX_NONE;
        if (!FromTileObj->TryGetNumberField(TEXT("col"), Col) ||
            !FromTileObj->TryGetNumberField(TEXT("row"), Row) || Col < 0 || Row < 0)
        {
            Ctx.SendError(ErrorCodes::ERR_INVALID_PARAMS,
                TEXT("'georeferenceFromTile' needs non-negative integer 'col' and 'row'"));
            return true;
        }
        FromTile = FTileIndex(Col, Row);
    }

    TSharedPtr<FJsonObject> GeoObject;
    if (!ResolveGeoreferenceObject(Ctx.GetObject(TEXT("georeference")),
        Ctx.GetString(TEXT("georeferenceFrom")), FromTile, GeoObject, ErrCode, ErrMsg))
    {
        Ctx.SendError(ErrCode, ErrMsg);
        return true;
    }

    FGeoreference Geo;
    if (!ParseGeoreference(GeoObject, Source.Width, Source.Height, /*bImageIsSingleTile=*/false,
        Geo, ErrCode, ErrMsg))
    {
        Ctx.SendError(ErrCode, ErrMsg);
        return true;
    }

    const int64 TilesRequested = Geo.Grid.TileCount();
    if (TilesRequested > static_cast<int64>(MaxTilesPerCall))
    {
        Ctx.SendError(ErrorCodes::ERR_TILE_BUDGET_EXCEEDED,
            FString::Printf(TEXT("%lld tiles (%d x %d) exceeds the %d-tile ceiling for one call. Every tile is a separate encode and file write on the game thread with no job handle to cancel; split the grid across several calls."),
                TilesRequested, Geo.Grid.Cols, Geo.Grid.Rows, MaxTilesPerCall));
        return true;
    }

    // ---- output targets, all resolved and checked BEFORE anything is written ----
    const FString Prefix = SanitizeBaseName(Ctx.GetString(TEXT("namePrefix")),
        SanitizeBaseName(FPaths::GetBaseFilename(SourcePath), TEXT("tile")));
    const FString OutputDir = ResolveOutputDir(Ctx.GetString(TEXT("outputDir")),
        FString(TEXT("tiles")) / Prefix);
    const bool bOverwrite = Ctx.GetBool(TEXT("overwrite"), false);

    const int32 ColDigits = FString::FromInt(FMath::Max(0, Geo.Grid.Cols - 1)).Len();
    const int32 RowDigits = FString::FromInt(FMath::Max(0, Geo.Grid.Rows - 1)).Len();
    const FString ManifestPath = OutputDir / (Prefix + TEXT("_manifest.json"));

    TArray<FString> TilePaths;
    TilePaths.Reserve(static_cast<int32>(TilesRequested));
    TArray<FString> Existing;
    for (int32 Row = 0; Row < Geo.Grid.Rows; ++Row)
    {
        for (int32 Col = 0; Col < Geo.Grid.Cols; ++Col)
        {
            const FString Path = OutputDir / PwTileFileName(Prefix, Col, Row, ColDigits, RowDigits);
            TilePaths.Add(Path);
            if (!bOverwrite && IFileManager::Get().FileExists(*Path))
            {
                Existing.Add(Path);
            }
        }
    }
    if (!bOverwrite && IFileManager::Get().FileExists(*ManifestPath))
    {
        Existing.Add(ManifestPath);
    }
    if (Existing.Num() > 0)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        TArray<TSharedPtr<FJsonValue>> Blocking;
        for (const FString& Path : Existing)
        {
            Blocking.Add(MakeShared<FJsonValueString>(Path));
        }
        Payload->SetArrayField(TEXT("existingFiles"), Blocking);
        Payload->SetStringField(TEXT("outputDir"), OutputDir);
        Ctx.SendError(ErrorCodes::ERR_ALREADY_EXISTS,
            FString::Printf(TEXT("%d output file(s) already exist under %s and overwrite is false. Nothing was written - pass overwrite:true, or choose another outputDir/namePrefix."),
                Existing.Num(), *OutputDir),
            Payload);
        return true;
    }

    IFileManager::Get().MakeDirectory(*OutputDir, true);

    // ---- cut and write ----
    TArray<TSharedPtr<FJsonValue>> TileEntries;
    TileEntries.Reserve(static_cast<int32>(TilesRequested));
    TSet<FString> WrittenNames;
    int32 TilesWritten = 0;

    for (int32 Row = 0; Row < Geo.Grid.Rows; ++Row)
    {
        for (int32 Col = 0; Col < Geo.Grid.Cols; ++Col)
        {
            const FTileIndex Tile(Col, Row);
            const int32 Flat = Row * Geo.Grid.Cols + Col;

            FBitmap TileBitmap;
            FString SliceErr;
            if (!ExtractTile(Source, Geo.Grid, Tile, TileBitmap, SliceErr))
            {
                Ctx.SendError(ErrorCodes::ERR_TILE_OUT_OF_RANGE,
                    FString::Printf(TEXT("Could not cut tile (%d,%d): %s. %d of %lld tiles were already written to %s."),
                        Col, Row, *SliceErr, TilesWritten, TilesRequested, *OutputDir));
                return true;
            }
            if (!SaveBitmapPng(TilePaths[Flat], TileBitmap, ErrCode, ErrMsg))
            {
                Ctx.SendError(ErrCode,
                    FString::Printf(TEXT("%s (tile %d,%d; %d of %lld tiles were already written to %s)"),
                        *ErrMsg, Col, Row, TilesWritten, TilesRequested, *OutputDir));
                return true;
            }
            ++TilesWritten;
            WrittenNames.Add(FPaths::GetCleanFilename(TilePaths[Flat]));

            const FVector2D Origin = TilePixelToPixel(Geo.Grid, Tile, FVector2D::ZeroVector);
            FVector TileMin = FVector::ZeroVector;
            FVector TileMax = FVector::ZeroVector;
            TSharedPtr<FJsonObject> TileGeo;
            if (!PwTileMakeTileGeoreference(Geo, Tile, TileMin, TileMax, TileGeo))
            {
                Ctx.SendError(ErrorCodes::ERR_TILE_OUT_OF_RANGE,
                    FString::Printf(TEXT("Tile (%d,%d) has no world extent in a %d x %d grid"),
                        Col, Row, Geo.Grid.Cols, Geo.Grid.Rows));
                return true;
            }

            TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
            Entry->SetNumberField(TEXT("col"), Col);
            Entry->SetNumberField(TEXT("row"), Row);
            Entry->SetStringField(TEXT("file"), FPaths::GetCleanFilename(TilePaths[Flat]));
            Entry->SetStringField(TEXT("path"), TilePaths[Flat]);
            Entry->SetNumberField(TEXT("pixelOriginX"), Origin.X);
            Entry->SetNumberField(TEXT("pixelOriginY"), Origin.Y);
            Entry->SetNumberField(TEXT("pixelWidth"), Geo.Grid.TilePixelWidth);
            Entry->SetNumberField(TEXT("pixelHeight"), Geo.Grid.TilePixelHeight);
            Entry->SetObjectField(TEXT("worldMin"), MakeVectorObject(TileMin));
            Entry->SetObjectField(TEXT("worldMax"), MakeVectorObject(TileMax));
            Entry->SetObjectField(TEXT("worldCentre"), MakeVectorObject((TileMin + TileMax) * 0.5));
            Entry->SetObjectField(TEXT("georeference"), TileGeo);
            TileEntries.Add(MakeShared<FJsonValueObject>(Entry));
        }
    }

    // ---- manifest ----
    TSharedPtr<FJsonObject> Manifest = MakeShared<FJsonObject>();
    Manifest->SetStringField(TEXT("schema"), TEXT("pinwright.image.tile/1"));
    Manifest->SetStringField(TEXT("generatedUtc"), FDateTime::UtcNow().ToIso8601());
    Manifest->SetStringField(TEXT("source"), SourcePath);
    Manifest->SetNumberField(TEXT("sourceWidth"), Source.Width);
    Manifest->SetNumberField(TEXT("sourceHeight"), Source.Height);
    Manifest->SetStringField(TEXT("namePrefix"), Prefix);
    Manifest->SetObjectField(TEXT("georeference"), SerializeGeoreference(Geo));
    Manifest->SetObjectField(TEXT("grid"), DescribeGrid(Geo));
    Manifest->SetArrayField(TEXT("tiles"), TileEntries);

    FString ManifestText;
    const TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer =
        TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&ManifestText);
    if (!FJsonSerializer::Serialize(Manifest.ToSharedRef(), Writer) ||
        !FFileHelper::SaveStringToFile(ManifestText, *ManifestPath))
    {
        Ctx.SendError(ErrorCodes::ERR_WRITE_FAILED,
            FString::Printf(TEXT("Wrote %d tiles to %s but could not write the manifest %s. The tiles on disk have no recorded georeference until this succeeds."),
                TilesWritten, *OutputDir, *ManifestPath));
        return true;
    }

    // ---- stale files: measured, reported, never deleted ----
    // Tiles left over from an earlier run at a different subdivision sit in the same directory
    // under the same prefix and are indistinguishable from this run's output to anything reading
    // the directory. Deleting them would be an unannounced mutation; naming them is the fix.
    TArray<FString> DirFiles;
    IFileManager::Get().FindFiles(DirFiles, *(OutputDir / (Prefix + TEXT("_r*c*.png"))), true, false);
    TArray<TSharedPtr<FJsonValue>> Stale;
    for (const FString& Name : DirFiles)
    {
        if (!WrittenNames.Contains(Name))
        {
            Stale.Add(MakeShared<FJsonValueString>(OutputDir / Name));
        }
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("source"), SourcePath);
    Result->SetNumberField(TEXT("sourceWidth"), Source.Width);
    Result->SetNumberField(TEXT("sourceHeight"), Source.Height);
    Result->SetStringField(TEXT("outputDir"), OutputDir);
    Result->SetStringField(TEXT("namePrefix"), Prefix);
    Result->SetStringField(TEXT("manifest"), ManifestPath);
    Result->SetNumberField(TEXT("tilesRequested"), static_cast<double>(TilesRequested));
    Result->SetNumberField(TEXT("tilesWritten"), TilesWritten);
    Result->SetObjectField(TEXT("georeference"), SerializeGeoreference(Geo));
    Result->SetObjectField(TEXT("grid"), DescribeGrid(Geo));
    Result->SetBoolField(TEXT("tilesInResponse"), TileEntries.Num() <= PwTileMaxTilesInResponse);
    if (TileEntries.Num() <= PwTileMaxTilesInResponse)
    {
        Result->SetArrayField(TEXT("tiles"), TileEntries);
    }
    if (Stale.Num() > 0)
    {
        Result->SetArrayField(TEXT("staleFiles"), Stale);
    }
    Result->SetNumberField(TEXT("staleFileCount"), Stale.Num());

    // Non-square pixels are legal here (nothing in this verb captures anything) but they are
    // almost always a georeference the caller got wrong: the world box's aspect does not match the
    // image's. Reported rather than refused, because a deliberately anamorphic reference is a real
    // if rare thing - and reported rather than silently accepted, because every world coordinate
    // read off these tiles inherits the mistake.
    if (!Geo.Grid.HasSquarePixels())
    {
        TArray<TSharedPtr<FJsonValue>> Warnings;
        Warnings.Add(MakeShared<FJsonValueString>(FString::Printf(
            TEXT("Non-square pixels: %.4f cm/px across (%s) vs %.4f cm/px down (%s). The world box's aspect does not match the image's, so a distance measured horizontally and one measured vertically are on different scales. Check worldMin/worldMax against the image dimensions."),
            Geo.Grid.WorldUnitsPerPixelAcross(), PinWrightTileGrid::AxisName(Geo.Grid.Axes.ScreenXAxis),
            Geo.Grid.WorldUnitsPerPixelDown(), PinWrightTileGrid::AxisName(Geo.Grid.Axes.ScreenYAxis))));
        Result->SetArrayField(TEXT("warnings"), Warnings);
    }

    Ctx.SendSuccess(FString::Printf(TEXT("Wrote %d of %lld tiles (%d x %d) to %s"),
        TilesWritten, TilesRequested, Geo.Grid.Cols, Geo.Grid.Rows, *OutputDir), Result);
    return true;
}
