// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/Image/ImageOps.h"

#include "Handlers/ErrorCodes.h"
#include "Utils/JsonUtils.h"
#include "Utils/ScreenshotUtils.h"

#include "HAL/FileManager.h"
#include "ImageCore.h"
#include "ImageUtils.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace PinWrightImage
{
    using namespace PinWrightTileGrid;

    namespace
    {
        // Named-namespace-free file locals are unsafe under Unity (CLAUDE.md), so everything here
        // is either static-local or carries the PwImage prefix inside this anonymous namespace,
        // which is itself nested inside the named PinWrightImage namespace.

        bool PwImageReadRequiredInt(const TSharedPtr<FJsonObject>& Obj, const FString& Field,
            int32 MinValue, int32 MaxValue, int32& Out, FString& OutErr)
        {
            double Raw = 0.0;
            if (!Obj->TryGetNumberField(Field, Raw))
            {
                OutErr = FString::Printf(TEXT("'%s' is required and must be a number"), *Field);
                return false;
            }
            if (Raw != FMath::TruncToDouble(Raw))
            {
                OutErr = FString::Printf(TEXT("'%s' must be a whole number, got %f"), *Field, Raw);
                return false;
            }
            if (Raw < static_cast<double>(MinValue) || Raw > static_cast<double>(MaxValue))
            {
                OutErr = FString::Printf(TEXT("'%s' must be between %d and %d, got %f"),
                    *Field, MinValue, MaxValue, Raw);
                return false;
            }
            Out = static_cast<int32>(Raw);
            return true;
        }

        bool PwImageParseAxisLetter(const FString& Text, EWorldAxis& Out, FString& OutErr)
        {
            const FString Trimmed = Text.TrimStartAndEnd();
            if (Trimmed.Equals(TEXT("X"), ESearchCase::IgnoreCase)) { Out = EWorldAxis::X; return true; }
            if (Trimmed.Equals(TEXT("Y"), ESearchCase::IgnoreCase)) { Out = EWorldAxis::Y; return true; }
            if (Trimmed.Equals(TEXT("Z"), ESearchCase::IgnoreCase)) { Out = EWorldAxis::Z; return true; }
            OutErr = FString::Printf(TEXT("world axis must be one of X, Y, Z; got '%s'"), *Text);
            return false;
        }

        // Preset name -> mapping. Normalizes '-' to '_' and case so a caller who types the name
        // out of the wiki cannot miss by punctuation, but an unrecognized name still ERRORS.
        bool PwImageParseAxesPreset(const FString& Name, FScreenAxisMapping& Out, FString& OutErr)
        {
            const FString Key = Name.TrimStartAndEnd().Replace(TEXT("-"), TEXT("_")).ToLower();
            if (Key == TEXT("top_down_x_up_y_right")) { Out = TopDownXUpYRight(); return true; }
            if (Key == TEXT("top_down_x_right_y_down")) { Out = TopDownXRightYDown(); return true; }
            if (Key == TEXT("front_y_right_z_up")) { Out = FrontYRightZUp(); return true; }
            if (Key == TEXT("side_x_right_z_up")) { Out = SideXRightZUp(); return true; }
            OutErr = FString::Printf(
                TEXT("unknown axes preset '%s'; expected one of top_down_x_up_y_right, ")
                TEXT("top_down_x_right_y_down, front_y_right_z_up, side_x_right_z_up, or the ")
                TEXT("explicit object form {screenXAxis, screenXPositive, screenYAxis, screenYPositive}"),
                *Name);
            return false;
        }

        bool PwImageNearlyEqual(double A, double B, double Tolerance)
        {
            return FMath::Abs(A - B) <= Tolerance;
        }

        bool PwImageIsAllHexDigits(const FString& Text)
        {
            for (const TCHAR Ch : Text)
            {
                if (!FChar::IsHexDigit(Ch))
                {
                    return false;
                }
            }
            return Text.Len() > 0;
        }
    }

    // -------------------------------------------------------------------------------------------
    // Bitmaps
    // -------------------------------------------------------------------------------------------

    bool LoadBitmap(const FString& AbsolutePath, FBitmap& Out, FString& OutErrCode, FString& OutErrMsg)
    {
        Out = FBitmap();
        if (AbsolutePath.IsEmpty())
        {
            OutErrCode = ErrorCodes::ERR_FILE_NOT_FOUND;
            OutErrMsg = TEXT("Empty image path");
            return false;
        }
        if (!IFileManager::Get().FileExists(*AbsolutePath))
        {
            OutErrCode = ErrorCodes::ERR_FILE_NOT_FOUND;
            OutErrMsg = FString::Printf(TEXT("No such image file: %s"), *AbsolutePath);
            return false;
        }

        FImage Loaded;
        if (!FImageUtils::LoadImage(*AbsolutePath, Loaded))
        {
            OutErrCode = ErrorCodes::ERR_DECODE_FAILED;
            OutErrMsg = FString::Printf(TEXT("Could not decode image: %s"), *AbsolutePath);
            return false;
        }
        Loaded.ChangeFormat(ERawImageFormat::BGRA8, EGammaSpace::sRGB);

        const int32 W = Loaded.SizeX;
        const int32 H = Loaded.SizeY;
        TArrayView64<FColor> View = Loaded.AsBGRA8();
        if (W <= 0 || H <= 0 || View.Num() < static_cast<int64>(W) * static_cast<int64>(H))
        {
            OutErrCode = ErrorCodes::ERR_DECODE_FAILED;
            OutErrMsg = FString::Printf(TEXT("Decoded image had no pixels: %s"), *AbsolutePath);
            return false;
        }
        if (static_cast<int64>(W) * static_cast<int64>(H) > static_cast<int64>(MAX_int32))
        {
            OutErrCode = ErrorCodes::ERR_DECODE_FAILED;
            OutErrMsg = FString::Printf(TEXT("%s is %d x %d, past the addressable pixel count"),
                *AbsolutePath, W, H);
            return false;
        }

        Out.Width = W;
        Out.Height = H;
        Out.Pixels.SetNumUninitialized(W * H);
        FMemory::Memcpy(Out.Pixels.GetData(), View.GetData(), sizeof(FColor) * static_cast<SIZE_T>(W) * H);
        OutErrCode.Reset();
        OutErrMsg.Reset();
        return true;
    }

    bool SaveBitmapPng(const FString& AbsolutePath, const FBitmap& In, FString& OutErrCode, FString& OutErrMsg)
    {
        if (!In.IsValid())
        {
            OutErrCode = ErrorCodes::ERR_ENCODE_FAILED;
            OutErrMsg = FString::Printf(TEXT("Refusing to write a degenerate bitmap (%d x %d, %d pixels) to %s"),
                In.Width, In.Height, In.Pixels.Num(), *AbsolutePath);
            return false;
        }

        TArray<uint8> Png;
        if (!PinWrightScreenshotUtils::EncodeBitmapToPng(In.Width, In.Height, In.Pixels, Png))
        {
            OutErrCode = ErrorCodes::ERR_ENCODE_FAILED;
            OutErrMsg = FString::Printf(TEXT("PNG encode failed for %s"), *AbsolutePath);
            return false;
        }

        const FString Dir = FPaths::GetPath(AbsolutePath);
        if (!Dir.IsEmpty())
        {
            IFileManager::Get().MakeDirectory(*Dir, true);
        }
        if (!FFileHelper::SaveArrayToFile(Png, *AbsolutePath))
        {
            OutErrCode = ErrorCodes::ERR_WRITE_FAILED;
            OutErrMsg = FString::Printf(TEXT("Could not write %s"), *AbsolutePath);
            return false;
        }
        OutErrCode.Reset();
        OutErrMsg.Reset();
        return true;
    }

    bool MakeBitmap(int32 Width, int32 Height, const FColor& Fill, FBitmap& Out, FString& OutErr)
    {
        if (Width <= 0 || Height <= 0)
        {
            OutErr = FString::Printf(TEXT("bitmap size must be positive, got %d x %d"), Width, Height);
            return false;
        }
        const int64 Count = static_cast<int64>(Width) * static_cast<int64>(Height);
        if (Count > static_cast<int64>(MAX_int32))
        {
            OutErr = FString::Printf(TEXT("bitmap %d x %d exceeds the addressable pixel count"), Width, Height);
            return false;
        }
        Out.Width = Width;
        Out.Height = Height;
        Out.Pixels.Init(Fill, static_cast<int32>(Count));
        return true;
    }

    bool CopyRegion(const FBitmap& Src, int32 OriginX, int32 OriginY, int32 W, int32 H,
        FBitmap& Out, FString& OutErr)
    {
        if (!Src.IsValid())
        {
            OutErr = TEXT("source bitmap is invalid");
            return false;
        }
        if (W <= 0 || H <= 0)
        {
            OutErr = FString::Printf(TEXT("region size must be positive, got %d x %d"), W, H);
            return false;
        }
        // Refuses instead of clamping: a short tile is a wrong tile, and it would read as a
        // successful cut of ground it does not cover.
        if (OriginX < 0 || OriginY < 0 || OriginX + W > Src.Width || OriginY + H > Src.Height)
        {
            OutErr = FString::Printf(
                TEXT("region [%d,%d)+(%d x %d) leaves the %d x %d source"),
                OriginX, OriginY, W, H, Src.Width, Src.Height);
            return false;
        }

        Out.Width = W;
        Out.Height = H;
        Out.Pixels.SetNumUninitialized(W * H);
        for (int32 Row = 0; Row < H; ++Row)
        {
            FMemory::Memcpy(Out.Pixels.GetData() + static_cast<SIZE_T>(Row) * W,
                Src.Pixels.GetData() + static_cast<SIZE_T>(OriginY + Row) * Src.Width + OriginX,
                sizeof(FColor) * static_cast<SIZE_T>(W));
        }
        return true;
    }

    bool BlitInto(FBitmap& Dst, const FBitmap& Src, int32 OriginX, int32 OriginY, FString& OutErr)
    {
        if (!Dst.IsValid() || !Src.IsValid())
        {
            OutErr = TEXT("source or destination bitmap is invalid");
            return false;
        }
        if (OriginX < 0 || OriginY < 0 ||
            OriginX + Src.Width > Dst.Width || OriginY + Src.Height > Dst.Height)
        {
            OutErr = FString::Printf(TEXT("a %d x %d blit at (%d,%d) leaves the %d x %d destination"),
                Src.Width, Src.Height, OriginX, OriginY, Dst.Width, Dst.Height);
            return false;
        }
        for (int32 Row = 0; Row < Src.Height; ++Row)
        {
            FMemory::Memcpy(Dst.Pixels.GetData() + static_cast<SIZE_T>(OriginY + Row) * Dst.Width + OriginX,
                Src.Pixels.GetData() + static_cast<SIZE_T>(Row) * Src.Width,
                sizeof(FColor) * static_cast<SIZE_T>(Src.Width));
        }
        return true;
    }

    bool ExtractTile(const FBitmap& Mosaic, const FTileGrid& Grid, const FTileIndex& Tile,
        FBitmap& Out, FString& OutErr)
    {
        if (!ValidateTileGrid(Grid, OutErr))
        {
            return false;
        }
        if (Tile.Col < 0 || Tile.Row < 0 || Tile.Col >= Grid.Cols || Tile.Row >= Grid.Rows)
        {
            OutErr = FString::Printf(TEXT("tile (%d,%d) is outside a %d x %d grid"),
                Tile.Col, Tile.Row, Grid.Cols, Grid.Rows);
            return false;
        }
        if (Mosaic.Width != Grid.ImageWidth() || Mosaic.Height != Grid.ImageHeight())
        {
            OutErr = FString::Printf(TEXT("mosaic is %d x %d but the grid describes %d x %d"),
                Mosaic.Width, Mosaic.Height, Grid.ImageWidth(), Grid.ImageHeight());
            return false;
        }

        // The georeference computes its own origin. Half-open [Origin, Origin + TileSize) matches
        // PixelToTilePixel, which puts a seam coordinate in the tile to its right / below.
        const FVector2D Origin = TilePixelToPixel(Grid, Tile, FVector2D::ZeroVector);
        return CopyRegion(Mosaic, static_cast<int32>(Origin.X), static_cast<int32>(Origin.Y),
            Grid.TilePixelWidth, Grid.TilePixelHeight, Out, OutErr);
    }

    // -------------------------------------------------------------------------------------------
    // Georeference
    // -------------------------------------------------------------------------------------------

    bool ParseAxes(const TSharedPtr<FJsonValue>& Value, FScreenAxisMapping& Out, FString& OutErr)
    {
        if (!Value.IsValid())
        {
            OutErr = TEXT("'axes' is required");
            return false;
        }

        FString PresetName;
        if (Value->TryGetString(PresetName))
        {
            return PwImageParseAxesPreset(PresetName, Out, OutErr);
        }

        const TSharedPtr<FJsonObject>* AsObject = nullptr;
        if (!Value->TryGetObject(AsObject) || !AsObject || !AsObject->IsValid())
        {
            OutErr = TEXT("'axes' must be a preset name or an object "
                "{screenXAxis, screenXPositive, screenYAxis, screenYPositive}");
            return false;
        }

        const TSharedPtr<FJsonObject>& Obj = *AsObject;
        const TArray<FString> Allowed = {
            TEXT("screenXAxis"), TEXT("screenXPositive"), TEXT("screenYAxis"), TEXT("screenYPositive") };
        if (!RejectUnknownKeys(Obj, Allowed, TEXT("axes"), OutErr))
        {
            return false;
        }

        FString XAxisText;
        FString YAxisText;
        bool bXPositive = false;
        bool bYPositive = false;
        if (!Obj->TryGetStringField(TEXT("screenXAxis"), XAxisText) ||
            !Obj->TryGetStringField(TEXT("screenYAxis"), YAxisText) ||
            !Obj->TryGetBoolField(TEXT("screenXPositive"), bXPositive) ||
            !Obj->TryGetBoolField(TEXT("screenYPositive"), bYPositive))
        {
            OutErr = TEXT("the explicit 'axes' form requires all four of screenXAxis (string), "
                "screenXPositive (bool), screenYAxis (string), screenYPositive (bool)");
            return false;
        }

        FScreenAxisMapping Mapping;
        if (!PwImageParseAxisLetter(XAxisText, Mapping.ScreenXAxis, OutErr) ||
            !PwImageParseAxisLetter(YAxisText, Mapping.ScreenYAxis, OutErr))
        {
            return false;
        }
        Mapping.bScreenXPositive = bXPositive;
        Mapping.bScreenYPositive = bYPositive;

        if (!Mapping.IsValid())
        {
            OutErr = FString::Printf(
                TEXT("screenXAxis and screenYAxis must be different world axes; both are %s, which ")
                TEXT("is not invertible - every pixel would deproject to a line"),
                AxisName(Mapping.ScreenXAxis));
            return false;
        }
        Out = Mapping;
        return true;
    }

    FString AxesPresetName(const FScreenAxisMapping& Mapping)
    {
        if (Mapping == TopDownXUpYRight())   { return TEXT("top_down_x_up_y_right"); }
        if (Mapping == TopDownXRightYDown()) { return TEXT("top_down_x_right_y_down"); }
        if (Mapping == FrontYRightZUp())     { return TEXT("front_y_right_z_up"); }
        if (Mapping == SideXRightZUp())      { return TEXT("side_x_right_z_up"); }
        return FString();
    }

    TSharedPtr<FJsonObject> DescribeAxes(const FScreenAxisMapping& Mapping)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("screenXAxis"), AxisName(Mapping.ScreenXAxis));
        Obj->SetBoolField(TEXT("screenXPositive"), Mapping.bScreenXPositive);
        Obj->SetStringField(TEXT("screenYAxis"), AxisName(Mapping.ScreenYAxis));
        Obj->SetBoolField(TEXT("screenYPositive"), Mapping.bScreenYPositive);
        return Obj;
    }

    TSharedPtr<FJsonValue> SerializeAxes(const FScreenAxisMapping& Mapping)
    {
        const FString Preset = AxesPresetName(Mapping);
        if (!Preset.IsEmpty())
        {
            return MakeShared<FJsonValueString>(Preset);
        }
        return MakeShared<FJsonValueObject>(DescribeAxes(Mapping));
    }

    bool ParseGeoreference(const TSharedPtr<FJsonObject>& Obj, int32 ImageWidth, int32 ImageHeight,
        bool bImageIsSingleTile, FGeoreference& Out, FString& OutErrCode, FString& OutErrMsg)
    {
        OutErrCode = ErrorCodes::ERR_INVALID_GEOREFERENCE;
        if (!Obj.IsValid())
        {
            OutErrMsg = TEXT("georeference must be an object");
            return false;
        }

        const TArray<FString> Allowed = {
            TEXT("axes"), TEXT("worldMin"), TEXT("worldMax"), TEXT("cols"), TEXT("rows"), TEXT("depthCm") };
        if (!RejectUnknownKeys(Obj, Allowed, TEXT("georeference"), OutErrMsg))
        {
            return false;
        }

        FGeoreference Geo;
        FScreenAxisMapping Mapping;
        if (!ParseAxes(Obj->TryGetField(TEXT("axes")), Mapping, OutErrMsg))
        {
            return false;
        }
        if (!ParseWorldPointField(Obj, TEXT("worldMin"), Geo.WorldMin, OutErrMsg) ||
            !ParseWorldPointField(Obj, TEXT("worldMax"), Geo.WorldMax, OutErrMsg))
        {
            return false;
        }

        int32 Cols = 0;
        int32 Rows = 0;
        if (!PwImageReadRequiredInt(Obj, TEXT("cols"), 1, MaxTilesPerAxis, Cols, OutErrMsg) ||
            !PwImageReadRequiredInt(Obj, TEXT("rows"), 1, MaxTilesPerAxis, Rows, OutErrMsg))
        {
            return false;
        }

        // The tile pixel size is DERIVED from the file in hand - never read off the wire. That is
        // what lets one georeference describe a reference image and a capture of different
        // resolutions without either being edited.
        int32 TilePixelWidth = 0;
        int32 TilePixelHeight = 0;
        if (bImageIsSingleTile)
        {
            TilePixelWidth = ImageWidth;
            TilePixelHeight = ImageHeight;
        }
        else
        {
            if (ImageWidth % Cols != 0 || ImageHeight % Rows != 0)
            {
                OutErrCode = ErrorCodes::ERR_IMAGE_GRID_MISMATCH;
                OutErrMsg = FString::Printf(
                    TEXT("a %d x %d image does not divide into %d x %d whole tiles (%d %% %d = %d, %d %% %d = %d). ")
                    TEXT("A partial edge tile would have a different world-units-per-pixel from its neighbours ")
                    TEXT("at exactly the seam a reader measures across, so it is refused rather than rounded. ")
                    TEXT("Choose cols/rows that divide the image, or re-export the image at a divisible size."),
                    ImageWidth, ImageHeight, Cols, Rows,
                    ImageWidth, Cols, ImageWidth % Cols, ImageHeight, Rows, ImageHeight % Rows);
                return false;
            }
            TilePixelWidth = ImageWidth / Cols;
            TilePixelHeight = ImageHeight / Rows;
        }

        const FWorldExtent2D Extent = MakeWorldExtentFromBounds(Geo.WorldMin, Geo.WorldMax, Mapping);
        if (!Extent.IsValid())
        {
            OutErrMsg = FString::Printf(
                TEXT("worldMin/worldMax span nothing in the capture plane (%s span %.3f cm, %s span %.3f cm). ")
                TEXT("Both in-plane axes need a strictly positive span; the depth axis (%s) may be flat."),
                AxisName(Mapping.ScreenXAxis), Extent.SpanAcross(),
                AxisName(Mapping.ScreenYAxis), Extent.SpanDown(),
                AxisName(Mapping.DepthAxis()));
            return false;
        }

        FString GridErr;
        if (!MakeTileGrid(Extent, Mapping, Cols, Rows, TilePixelWidth, TilePixelHeight, Geo.Grid, GridErr))
        {
            OutErrMsg = FString::Printf(TEXT("georeference does not describe a usable tile grid: %s"), *GridErr);
            return false;
        }

        // Depth: the midpoint of the AABB's depth span unless the caller overrides it. A
        // definition rather than a heuristic, and reported on every response so it is never
        // implicit in a coordinate the caller reads back.
        const double DepthMin = GetAxisValue(Geo.WorldMin, Mapping.DepthAxis());
        const double DepthMax = GetAxisValue(Geo.WorldMax, Mapping.DepthAxis());
        Geo.DepthCm = 0.5 * (DepthMin + DepthMax);
        double ExplicitDepth = 0.0;
        if (Obj->TryGetNumberField(TEXT("depthCm"), ExplicitDepth))
        {
            Geo.DepthCm = ExplicitDepth;
            Geo.bDepthExplicit = true;
        }

        Out = Geo;
        OutErrCode.Reset();
        OutErrMsg.Reset();
        return true;
    }

    TSharedPtr<FJsonObject> SerializeGeoreference(const FGeoreference& Geo)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetField(TEXT("axes"), SerializeAxes(Geo.Grid.Axes));
        Obj->SetObjectField(TEXT("worldMin"), MakeVectorObject(Geo.WorldMin));
        Obj->SetObjectField(TEXT("worldMax"), MakeVectorObject(Geo.WorldMax));
        Obj->SetNumberField(TEXT("cols"), Geo.Grid.Cols);
        Obj->SetNumberField(TEXT("rows"), Geo.Grid.Rows);
        Obj->SetNumberField(TEXT("depthCm"), Geo.DepthCm);
        return Obj;
    }

    TSharedPtr<FJsonObject> DescribeGrid(const FGeoreference& Geo)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetNumberField(TEXT("cols"), Geo.Grid.Cols);
        Obj->SetNumberField(TEXT("rows"), Geo.Grid.Rows);
        Obj->SetNumberField(TEXT("tilePixelWidth"), Geo.Grid.TilePixelWidth);
        Obj->SetNumberField(TEXT("tilePixelHeight"), Geo.Grid.TilePixelHeight);
        Obj->SetNumberField(TEXT("imageWidth"), Geo.Grid.ImageWidth());
        Obj->SetNumberField(TEXT("imageHeight"), Geo.Grid.ImageHeight());
        Obj->SetNumberField(TEXT("worldUnitsPerPixelAcross"), Geo.Grid.WorldUnitsPerPixelAcross());
        Obj->SetNumberField(TEXT("worldUnitsPerPixelDown"), Geo.Grid.WorldUnitsPerPixelDown());
        Obj->SetBoolField(TEXT("squarePixels"), Geo.Grid.HasSquarePixels());
        Obj->SetObjectField(TEXT("axes"), DescribeAxes(Geo.Grid.Axes));
        Obj->SetStringField(TEXT("acrossAxis"), AxisName(Geo.Grid.Axes.ScreenXAxis));
        Obj->SetStringField(TEXT("downAxis"), AxisName(Geo.Grid.Axes.ScreenYAxis));
        Obj->SetStringField(TEXT("depthAxis"), AxisName(Geo.Grid.Axes.DepthAxis()));
        Obj->SetNumberField(TEXT("depthCm"), Geo.DepthCm);
        Obj->SetBoolField(TEXT("depthExplicit"), Geo.bDepthExplicit);
        Obj->SetNumberField(TEXT("worldSpanAcrossCm"), Geo.Grid.World.SpanAcross());
        Obj->SetNumberField(TEXT("worldSpanDownCm"), Geo.Grid.World.SpanDown());
        return Obj;
    }

    bool GeoreferencesAgree(const FGeoreference& A, const FGeoreference& B, FString& OutWhy)
    {
        constexpr double Tolerance = 1.0e-4; // one micrometre, well inside double precision on cm

        if (A.Grid.Axes != B.Grid.Axes)
        {
            OutWhy = FString::Printf(
                TEXT("axis mapping differs: A maps screen X to %s%s / screen Y to %s%s, ")
                TEXT("B maps screen X to %s%s / screen Y to %s%s"),
                A.Grid.Axes.bScreenXPositive ? TEXT("+") : TEXT("-"), AxisName(A.Grid.Axes.ScreenXAxis),
                A.Grid.Axes.bScreenYPositive ? TEXT("+") : TEXT("-"), AxisName(A.Grid.Axes.ScreenYAxis),
                B.Grid.Axes.bScreenXPositive ? TEXT("+") : TEXT("-"), AxisName(B.Grid.Axes.ScreenXAxis),
                B.Grid.Axes.bScreenYPositive ? TEXT("+") : TEXT("-"), AxisName(B.Grid.Axes.ScreenYAxis));
            return false;
        }
        if (!PwImageNearlyEqual(A.Grid.World.Min.X, B.Grid.World.Min.X, Tolerance) ||
            !PwImageNearlyEqual(A.Grid.World.Min.Y, B.Grid.World.Min.Y, Tolerance) ||
            !PwImageNearlyEqual(A.Grid.World.Max.X, B.Grid.World.Max.X, Tolerance) ||
            !PwImageNearlyEqual(A.Grid.World.Max.Y, B.Grid.World.Max.Y, Tolerance))
        {
            OutWhy = FString::Printf(
                TEXT("in-plane world extent differs: A covers %s [%.3f, %.3f] x %s [%.3f, %.3f], ")
                TEXT("B covers %s [%.3f, %.3f] x %s [%.3f, %.3f] (cm)"),
                AxisName(A.Grid.Axes.ScreenXAxis), A.Grid.World.Min.X, A.Grid.World.Max.X,
                AxisName(A.Grid.Axes.ScreenYAxis), A.Grid.World.Min.Y, A.Grid.World.Max.Y,
                AxisName(B.Grid.Axes.ScreenXAxis), B.Grid.World.Min.X, B.Grid.World.Max.X,
                AxisName(B.Grid.Axes.ScreenYAxis), B.Grid.World.Min.Y, B.Grid.World.Max.Y);
            return false;
        }
        if (A.Grid.Cols != B.Grid.Cols || A.Grid.Rows != B.Grid.Rows)
        {
            OutWhy = FString::Printf(TEXT("subdivision differs: A is %d x %d tiles, B is %d x %d tiles"),
                A.Grid.Cols, A.Grid.Rows, B.Grid.Cols, B.Grid.Rows);
            return false;
        }
        if (!PwImageNearlyEqual(A.DepthCm, B.DepthCm, Tolerance))
        {
            OutWhy = FString::Printf(TEXT("depth plane differs: A is at %s = %.3f cm, B is at %s = %.3f cm"),
                AxisName(A.Grid.Axes.DepthAxis()), A.DepthCm,
                AxisName(B.Grid.Axes.DepthAxis()), B.DepthCm);
            return false;
        }
        OutWhy.Reset();
        return true;
    }

    bool LoadGeoreferenceObjectFromFile(const FString& AbsolutePath, const FTileIndex& TileSelector,
        TSharedPtr<FJsonObject>& Out, FString& OutErrCode, FString& OutErrMsg)
    {
        Out.Reset();
        if (!IFileManager::Get().FileExists(*AbsolutePath))
        {
            OutErrCode = ErrorCodes::ERR_FILE_NOT_FOUND;
            OutErrMsg = FString::Printf(TEXT("No such georeference/manifest file: %s"), *AbsolutePath);
            return false;
        }

        FString Text;
        if (!FFileHelper::LoadFileToString(Text, *AbsolutePath))
        {
            OutErrCode = ErrorCodes::ERR_INVALID_MANIFEST;
            OutErrMsg = FString::Printf(TEXT("Could not read %s"), *AbsolutePath);
            return false;
        }

        TSharedPtr<FJsonObject> Root;
        const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
        if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
        {
            OutErrCode = ErrorCodes::ERR_INVALID_MANIFEST;
            OutErrMsg = FString::Printf(TEXT("%s is not a JSON object"), *AbsolutePath);
            return false;
        }

        if (TileSelector.IsSet())
        {
            const TArray<TSharedPtr<FJsonValue>>* Tiles = nullptr;
            if (!Root->TryGetArrayField(TEXT("tiles"), Tiles) || !Tiles)
            {
                OutErrCode = ErrorCodes::ERR_INVALID_MANIFEST;
                OutErrMsg = FString::Printf(
                    TEXT("%s has no 'tiles' array, so tile (%d,%d) cannot be selected from it"),
                    *AbsolutePath, TileSelector.Col, TileSelector.Row);
                return false;
            }
            for (const TSharedPtr<FJsonValue>& Entry : *Tiles)
            {
                const TSharedPtr<FJsonObject>* TileObj = nullptr;
                if (!Entry.IsValid() || !Entry->TryGetObject(TileObj) || !TileObj) { continue; }
                int32 Col = INDEX_NONE;
                int32 Row = INDEX_NONE;
                if (!(*TileObj)->TryGetNumberField(TEXT("col"), Col) ||
                    !(*TileObj)->TryGetNumberField(TEXT("row"), Row))
                {
                    continue;
                }
                if (Col != TileSelector.Col || Row != TileSelector.Row) { continue; }
                const TSharedPtr<FJsonObject>* Geo = nullptr;
                if (!(*TileObj)->TryGetObjectField(TEXT("georeference"), Geo) || !Geo)
                {
                    OutErrCode = ErrorCodes::ERR_INVALID_MANIFEST;
                    OutErrMsg = FString::Printf(TEXT("tile (%d,%d) in %s carries no 'georeference' object"),
                        TileSelector.Col, TileSelector.Row, *AbsolutePath);
                    return false;
                }
                Out = *Geo;
                OutErrCode.Reset();
                OutErrMsg.Reset();
                return true;
            }
            OutErrCode = ErrorCodes::ERR_INVALID_MANIFEST;
            OutErrMsg = FString::Printf(TEXT("%s lists no tile (%d,%d)"),
                *AbsolutePath, TileSelector.Col, TileSelector.Row);
            return false;
        }

        const TSharedPtr<FJsonObject>* Nested = nullptr;
        if (Root->TryGetObjectField(TEXT("georeference"), Nested) && Nested)
        {
            Out = *Nested;
        }
        else
        {
            // A bare georeference file: the root object IS the georeference. ParseGeoreference
            // rejects unknown keys, so a manifest-shaped file that lost its `georeference` member
            // fails loudly there rather than being half-read here.
            Out = Root;
        }
        OutErrCode.Reset();
        OutErrMsg.Reset();
        return true;
    }

    bool ResolveGeoreferenceObject(const TSharedPtr<FJsonObject>& InlineObject, const FString& FromPath,
        const FTileIndex& TileSelector, TSharedPtr<FJsonObject>& Out, FString& OutErrCode, FString& OutErrMsg)
    {
        Out.Reset();
        const bool bHasInline = InlineObject.IsValid();
        const bool bHasFile = !FromPath.TrimStartAndEnd().IsEmpty();

        if (bHasInline == bHasFile)
        {
            OutErrCode = ErrorCodes::ERR_INVALID_GEOREFERENCE;
            OutErrMsg = bHasInline
                ? TEXT("Supply exactly one of 'georeference' and 'georeferenceFrom'; both were given, and "
                       "picking one silently would let a stale manifest override a corrected inline object.")
                : TEXT("Supply exactly one of 'georeference' (an inline object) and 'georeferenceFrom' "
                       "(a path to an image.tile manifest or a bare georeference JSON file). Neither was given.");
            return false;
        }

        if (bHasInline)
        {
            Out = InlineObject;
            OutErrCode.Reset();
            OutErrMsg.Reset();
            return true;
        }
        return LoadGeoreferenceObjectFromFile(ResolveInputPath(FromPath), TileSelector, Out, OutErrCode, OutErrMsg);
    }

    // -------------------------------------------------------------------------------------------
    // Small shared parsers / builders
    // -------------------------------------------------------------------------------------------

    bool ParseColorString(const FString& Text, FColor& Out, FString& OutErr)
    {
        const FString Trimmed = Text.TrimStartAndEnd();
        if (Trimmed.IsEmpty())
        {
            OutErr = TEXT("colour is empty");
            return false;
        }

        FString Hex = Trimmed.StartsWith(TEXT("#")) ? Trimmed.RightChop(1) : Trimmed;
        const bool bLooksHex = (Hex.Len() == 6 || Hex.Len() == 8) && PwImageIsAllHexDigits(Hex);
        if (bLooksHex)
        {
            const uint32 Value = static_cast<uint32>(FParse::HexNumber(*Hex.Left(6)));
            Out.R = static_cast<uint8>((Value >> 16) & 0xFF);
            Out.G = static_cast<uint8>((Value >> 8) & 0xFF);
            Out.B = static_cast<uint8>(Value & 0xFF);
            Out.A = Hex.Len() == 8 ? static_cast<uint8>(FParse::HexNumber(*Hex.Mid(6, 2))) : 255;
            return true;
        }

        const FString Name = Trimmed.ToLower();
        if (Name == TEXT("white"))   { Out = FColor(255, 255, 255, 255); return true; }
        if (Name == TEXT("black"))   { Out = FColor(0, 0, 0, 255);       return true; }
        if (Name == TEXT("red"))     { Out = FColor(255, 48, 48, 255);   return true; }
        if (Name == TEXT("green"))   { Out = FColor(48, 255, 48, 255);   return true; }
        if (Name == TEXT("blue"))    { Out = FColor(64, 128, 255, 255);  return true; }
        if (Name == TEXT("yellow"))  { Out = FColor(255, 224, 32, 255);  return true; }
        if (Name == TEXT("cyan"))    { Out = FColor(0, 224, 255, 255);   return true; }
        if (Name == TEXT("magenta")) { Out = FColor(255, 0, 255, 255);   return true; }
        if (Name == TEXT("orange"))  { Out = FColor(255, 144, 16, 255);  return true; }
        if (Name == TEXT("grey") || Name == TEXT("gray")) { Out = FColor(160, 160, 160, 255); return true; }

        OutErr = FString::Printf(
            TEXT("unknown colour '%s'; expected #RRGGBB, #RRGGBBAA, or one of ")
            TEXT("white, black, red, green, blue, yellow, cyan, magenta, orange, grey"),
            *Text);
        return false;
    }

    bool ParseWorldPoint(const TSharedPtr<FJsonValue>& Value, FVector& Out, FString& OutErr)
    {
        if (!Value.IsValid())
        {
            OutErr = TEXT("world point is missing");
            return false;
        }

        const TArray<TSharedPtr<FJsonValue>>* AsArray = nullptr;
        if (Value->TryGetArray(AsArray) && AsArray)
        {
            if (AsArray->Num() < 2)
            {
                OutErr = TEXT("array form of a world point needs at least [x, y]");
                return false;
            }
            double Components[3] = { 0.0, 0.0, 0.0 };
            for (int32 Index = 0; Index < FMath::Min(3, AsArray->Num()); ++Index)
            {
                if (!(*AsArray)[Index].IsValid() || !(*AsArray)[Index]->TryGetNumber(Components[Index]))
                {
                    OutErr = FString::Printf(TEXT("world point component %d is not a number"), Index);
                    return false;
                }
            }
            Out = FVector(Components[0], Components[1], Components[2]);
            return true;
        }

        const TSharedPtr<FJsonObject>* AsObject = nullptr;
        if (!Value->TryGetObject(AsObject) || !AsObject || !AsObject->IsValid())
        {
            OutErr = TEXT("world point must be {x, y, z} or [x, y, z]");
            return false;
        }

        const TArray<FString> Allowed = { TEXT("x"), TEXT("y"), TEXT("z") };
        if (!RejectUnknownKeys(*AsObject, Allowed, TEXT("world point"), OutErr))
        {
            return false;
        }
        double X = 0.0;
        double Y = 0.0;
        double Z = 0.0;
        if (!(*AsObject)->TryGetNumberField(TEXT("x"), X) || !(*AsObject)->TryGetNumberField(TEXT("y"), Y))
        {
            OutErr = TEXT("world point needs numeric 'x' and 'y' (z defaults to 0)");
            return false;
        }
        (*AsObject)->TryGetNumberField(TEXT("z"), Z);
        Out = FVector(X, Y, Z);
        return true;
    }

    bool ParseWorldPointField(const TSharedPtr<FJsonObject>& Obj, const FString& Field, FVector& Out,
        FString& OutErr)
    {
        if (!Obj.IsValid())
        {
            OutErr = FString::Printf(TEXT("'%s' is missing"), *Field);
            return false;
        }
        const TSharedPtr<FJsonValue> Value = Obj->TryGetField(Field);
        if (!Value.IsValid())
        {
            OutErr = FString::Printf(TEXT("'%s' is required"), *Field);
            return false;
        }
        FString Inner;
        if (!ParseWorldPoint(Value, Out, Inner))
        {
            OutErr = FString::Printf(TEXT("'%s': %s"), *Field, *Inner);
            return false;
        }
        return true;
    }

    bool RejectUnknownKeys(const TSharedPtr<FJsonObject>& Obj, const TArray<FString>& Allowed,
        const FString& ContextName, FString& OutErr)
    {
        TArray<FString> Unknown;
        if (::RejectUnknownKeys(Obj, Allowed, Unknown, ERejectUnknownKeysMode::AllSorted))
        {
            return true;
        }
        OutErr = FString::Printf(TEXT("unknown key(s) in %s: [%s]. Valid keys: [%s]"),
            *ContextName, *FString::Join(Unknown, TEXT(", ")), *FString::Join(Allowed, TEXT(", ")));
        return false;
    }

    TSharedPtr<FJsonObject> MakeVectorObject(const FVector& V)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetNumberField(TEXT("x"), V.X);
        Obj->SetNumberField(TEXT("y"), V.Y);
        Obj->SetNumberField(TEXT("z"), V.Z);
        return Obj;
    }

    TSharedPtr<FJsonObject> MakePixelObject(const FVector2D& P)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetNumberField(TEXT("x"), P.X);
        Obj->SetNumberField(TEXT("y"), P.Y);
        return Obj;
    }

    // -------------------------------------------------------------------------------------------
    // Paths
    // -------------------------------------------------------------------------------------------

    FString ResolveInputPath(const FString& Requested)
    {
        FString Path = Requested.TrimStartAndEnd();
        if (Path.IsEmpty())
        {
            return Path;
        }
        FPaths::NormalizeFilename(Path);
        if (FPaths::IsRelative(Path))
        {
            Path = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir(), Path);
        }
        else
        {
            Path = FPaths::ConvertRelativePathToFull(Path);
        }
        return Path;
    }

    FString ResolveOutputDir(const FString& RequestedDir, const FString& DefaultSubdir)
    {
        FString Dir = RequestedDir.TrimStartAndEnd();
        if (Dir.IsEmpty())
        {
            Dir = FPaths::ProjectSavedDir() / TEXT("PinWright") / TEXT("image") / DefaultSubdir;
        }
        FPaths::NormalizeDirectoryName(Dir);
        if (FPaths::IsRelative(Dir))
        {
            Dir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir(), Dir);
        }
        else
        {
            Dir = FPaths::ConvertRelativePathToFull(Dir);
        }
        return Dir;
    }

    FString SanitizeBaseName(const FString& Requested, const FString& Fallback)
    {
        FString Name = FPaths::GetCleanFilename(Requested.TrimStartAndEnd());
        Name = FPaths::GetBaseFilename(Name);
        if (Name.Contains(TEXT("..")) || Name.Contains(TEXT("/")) || Name.Contains(TEXT("\\")))
        {
            Name.Reset();
        }
        // Keep the set narrow enough that the name is safe on every filesystem and still readable
        // in a directory listing next to 63 siblings.
        FString Cleaned;
        Cleaned.Reserve(Name.Len());
        for (const TCHAR Ch : Name)
        {
            if (FChar::IsAlnum(Ch) || Ch == TEXT('_') || Ch == TEXT('-') || Ch == TEXT('.'))
            {
                Cleaned.AppendChar(Ch);
            }
            else
            {
                Cleaned.AppendChar(TEXT('_'));
            }
        }
        Cleaned.TrimStartAndEndInline();
        return Cleaned.IsEmpty() ? Fallback : Cleaned;
    }
}
