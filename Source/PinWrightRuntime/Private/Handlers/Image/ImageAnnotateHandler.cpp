// Copyright (c) 2026 Alexander Penkin. MIT License.

// image.annotate - draw a world-aligned grid, crosshairs, boxes and labels onto an image, with
// EVERY position given in world coordinates.
//
// This is the verb that closes the measure loop: estimate a world position, draw it back onto the
// reference, look at it, correct. Removing the caller's hand-rolled pixel arithmetic is the entire
// point - the measured motivation is an eyeballed coordinate that was ~1188 world units out.
//
// TWO THINGS THIS VERB REFUSES TO DECIDE FOR THE CALLER
//
//  * It does not detect anything. It draws exactly what it was told to draw, at the pixel the
//    georeference puts it at, and reports that pixel back. A reader judges the result.
//  * It does not report a mark as drawn unless the pixels under it actually changed. `drawn` comes
//    from hashing the mark's own clipped rect before and after painting it, so an off-frame mark,
//    a zero-coverage mark and a mark whose colour equals the background all report false instead
//    of the request being echoed back as an outcome (rpc-design.md §1).
//
// The grid overlay composites at low alpha and puts its labels on the frame edges. That is not
// taste: a prototype grid at full opacity BURIED the detail it was drawn to measure (see the
// header note in Render/BitmapPaint.h). Two crossing translucent lines are darker where they
// cross - that is the honest visual and is deliberately not corrected.

#include "Handlers/HandlerContext.h"
#include "Handlers/HandlerRegistration.h"
#include "Handlers/ParamSpec.h"
#include "Handlers/ErrorCodes.h"
#include "Handlers/Image/ImageOps.h"
#include "Handlers/Render/BitmapPaint.h"
#include "Handlers/Render/TileGridUtils.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"

// No file-scope `using namespace` - Unity merges translation units and a using-directive outside a
// function body leaks into every .cpp compiled after it in the same blob.
namespace
{
    // Clip an inclusive pixel rect to the canvas. Returns false when nothing survives, which is
    // how an entirely off-frame mark becomes drawn:false instead of a silent no-op.
    bool PwAnnotateClipRect(int32 MinX, int32 MinY, int32 MaxX, int32 MaxY, int32 Width, int32 Height,
        FIntRect& Out)
    {
        const int32 X0 = FMath::Max(MinX, 0);
        const int32 Y0 = FMath::Max(MinY, 0);
        const int32 X1 = FMath::Min(MaxX, Width - 1);
        const int32 Y1 = FMath::Min(MaxY, Height - 1);
        if (X0 > X1 || Y0 > Y1)
        {
            return false;
        }
        Out = FIntRect(X0, Y0, X1 + 1, Y1 + 1); // FIntRect Max is exclusive
        return true;
    }

    // Order-sensitive hash over a clipped region. Only used to answer "did these pixels change",
    // so a cheap mixing function is enough and a collision would have to be engineered.
    uint64 PwAnnotateHashRegion(const PinWrightImage::FBitmap& Bitmap, const FIntRect& Rect)
    {
        uint64 Hash = 1469598103934665603ull;
        for (int32 Y = Rect.Min.Y; Y < Rect.Max.Y; ++Y)
        {
            const FColor* Row = Bitmap.Pixels.GetData() + static_cast<SIZE_T>(Y) * Bitmap.Width;
            for (int32 X = Rect.Min.X; X < Rect.Max.X; ++X)
            {
                Hash ^= static_cast<uint64>(Row[X].ToPackedARGB());
                Hash *= 1099511628211ull;
            }
        }
        return Hash;
    }

    // Shared per-mark styling parsed off a JSON entry.
    struct FPwAnnotateStyle
    {
        FColor Color = FColor::White;
        float Opacity = 1.0f;
        int32 ThicknessPx = 2;
    };

    bool PwAnnotateReadStyle(const TSharedPtr<FJsonObject>& Obj, const FString& Context,
        const TCHAR* DefaultColor, float DefaultOpacity, int32 DefaultThickness,
        FPwAnnotateStyle& Out, FString& OutErr)
    {
        FString ColorText = DefaultColor;
        Obj->TryGetStringField(TEXT("color"), ColorText);
        if (!PinWrightImage::ParseColorString(ColorText, Out.Color, OutErr))
        {
            OutErr = FString::Printf(TEXT("%s: %s"), *Context, *OutErr);
            return false;
        }

        double Opacity = DefaultOpacity;
        Obj->TryGetNumberField(TEXT("opacity"), Opacity);
        if (Opacity <= 0.0 || Opacity > 1.0)
        {
            OutErr = FString::Printf(
                TEXT("%s: 'opacity' must be greater than 0 and at most 1, got %f. A zero-coverage mark ")
                TEXT("draws nothing, which would be reported as drawn:false rather than refused - so it ")
                TEXT("is refused here instead."), *Context, Opacity);
            return false;
        }
        Out.Opacity = static_cast<float>(Opacity);

        int32 Thickness = DefaultThickness;
        Obj->TryGetNumberField(TEXT("thicknessPx"), Thickness);
        if (Thickness < 1 || Thickness > 64)
        {
            OutErr = FString::Printf(TEXT("%s: 'thicknessPx' must be between 1 and 64, got %d"),
                *Context, Thickness);
            return false;
        }
        Out.ThicknessPx = Thickness;
        return true;
    }

    // World point from flat x/y/z fields on a mark entry.
    bool PwAnnotateReadPoint(const TSharedPtr<FJsonObject>& Obj, const FString& Context, FVector& Out,
        FString& OutErr)
    {
        double X = 0.0;
        double Y = 0.0;
        double Z = 0.0;
        if (!Obj->TryGetNumberField(TEXT("x"), X) || !Obj->TryGetNumberField(TEXT("y"), Y))
        {
            OutErr = FString::Printf(TEXT("%s: needs numeric 'x' and 'y' (world centimetres); 'z' is optional"),
                *Context);
            return false;
        }
        Obj->TryGetNumberField(TEXT("z"), Z);
        Out = FVector(X, Y, Z);
        return true;
    }
}

REGISTER_RPC_HANDLER("image.annotate", "image",
    "Draw a world-aligned grid, crosshairs, boxes and labels onto an image or one of its tiles, with every position given in WORLD coordinates; reports the pixel each mark landed on and whether it actually changed pixels.",
    RPC_PARAMS(
        RPC_PARAM_REQ("image", "filepath",
            "Path to the image to annotate. Absolute, or relative to the project directory. The source file is never modified - the annotated copy is written to a new path."),
        RPC_PARAM_OPT("georeference", "object",
            "The ground this image covers: {axes, worldMin, worldMax, cols, rows, depthCm?}. Same object image.tile takes and writes; paste a tile's `georeference` straight out of a manifest. Use cols:1, rows:1 for an untiled image. Exactly one of georeference / georeferenceFrom."),
        RPC_PARAM_OPT("georeferenceFrom", "filepath",
            "Path to an image.tile manifest (or a bare georeference JSON file) whose georeference this call adopts. Exactly one of georeference / georeferenceFrom."),
        RPC_PARAM_OPT("georeferenceFromTile", "object",
            "{col, row} - take that tile's standalone georeference out of the manifest named by georeferenceFrom."),
        RPC_PARAM_OPT("tile", "object",
            "{col, row} - the supplied image is THAT TILE of the georeference's grid, not the whole mosaic. World coordinates then land at the same ground they would on the full image; a coordinate outside this tile is reported inFrame:false rather than clamped. Omit when annotating the whole image."),
        RPC_PARAM_OPT("snapCm", "number",
            "When greater than 0, every crosshair/box/label world point is snapped to this world grid before projection (nearest multiple, ties toward +infinity), and the response reports the requested and the snapped point separately. Must be positive when supplied."),
        RPC_PARAM_OPT("grid", "object",
            "{spacingCm (required), color, opacity, thicknessPx, labels} - a world-aligned grid across the frame. opacity defaults to 0.25 and labels sit on the frame edges: a full-opacity grid buries the detail it was drawn to measure. Two crossing translucent lines are darker where they cross; that is the honest composite."),
        RPC_PARAM_OPT("crosshairs", "array",
            "[{x, y, z?, label?, color?, opacity?, sizePx?, thicknessPx?}] - a cross centred on each world point."),
        RPC_PARAM_OPT("boxes", "array",
            "[{min:{x,y,z?}, max:{x,y,z?}, label?, color?, opacity?, thicknessPx?}] - an axis-aligned world box stroked as a pixel rect."),
        RPC_PARAM_OPT("labels", "array",
            "[{x, y, z?, text (required), color?, opacity?, scale?}] - a text plate anchored at a world point."),
        RPC_PARAM_OPT("outputDir", "filepath",
            "Directory for the annotated PNG. Defaults to <ProjectSaved>/PinWright/image/annotated."),
        RPC_PARAM_OPT("outputName", "string",
            "Filename stem for the annotated PNG. Defaults to <source base name>_annotated (plus _r<row>c<col> in tile mode)."),
        RPC_PARAM_DEF("overwrite", "boolean",
            "Replace an existing file at the target path. When false (the default) the call refuses before drawing anything.",
            "false")
    ))
{
    using namespace PinWrightImage;
    using namespace PinWrightTileGrid;
    using namespace PinWrightBitmapPaint;

    FString RequestedImage;
    if (!Ctx.RequireString(TEXT("image"), RequestedImage))
    {
        return true;
    }

    const FString SourcePath = ResolveInputPath(RequestedImage);
    FBitmap Original;
    FString ErrCode;
    FString ErrMsg;
    if (!LoadBitmap(SourcePath, Original, ErrCode, ErrMsg))
    {
        Ctx.SendError(ErrCode, ErrMsg);
        return true;
    }

    // ---- which tile (if any) this image is, and which tile of a manifest to read ----
    auto ReadTileIndex = [&Ctx](const TCHAR* Param, FTileIndex& Out, FString& OutErr) -> bool
    {
        const TSharedPtr<FJsonObject> Obj = Ctx.GetObject(Param);
        if (!Obj.IsValid())
        {
            return true;
        }
        const TArray<FString> Allowed = { TEXT("col"), TEXT("row") };
        if (!RejectUnknownKeys(Obj, Allowed, Param, OutErr))
        {
            return false;
        }
        int32 Col = INDEX_NONE;
        int32 Row = INDEX_NONE;
        if (!Obj->TryGetNumberField(TEXT("col"), Col) || !Obj->TryGetNumberField(TEXT("row"), Row) ||
            Col < 0 || Row < 0)
        {
            OutErr = FString::Printf(TEXT("'%s' needs non-negative integer 'col' and 'row'"), Param);
            return false;
        }
        Out = FTileIndex(Col, Row);
        return true;
    };

    FTileIndex FromTile;
    FTileIndex CanvasTile;
    FString ParamErr;
    if (!ReadTileIndex(TEXT("georeferenceFromTile"), FromTile, ParamErr) ||
        !ReadTileIndex(TEXT("tile"), CanvasTile, ParamErr))
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_PARAMS, ParamErr);
        return true;
    }
    const bool bTileMode = CanvasTile.IsSet();

    TSharedPtr<FJsonObject> GeoObject;
    if (!ResolveGeoreferenceObject(Ctx.GetObject(TEXT("georeference")),
        Ctx.GetString(TEXT("georeferenceFrom")), FromTile, GeoObject, ErrCode, ErrMsg))
    {
        Ctx.SendError(ErrCode, ErrMsg);
        return true;
    }

    FGeoreference Geo;
    if (!ParseGeoreference(GeoObject, Original.Width, Original.Height, bTileMode, Geo, ErrCode, ErrMsg))
    {
        Ctx.SendError(ErrCode, ErrMsg);
        return true;
    }
    if (bTileMode && (CanvasTile.Col >= Geo.Grid.Cols || CanvasTile.Row >= Geo.Grid.Rows))
    {
        Ctx.SendError(ErrorCodes::ERR_TILE_OUT_OF_RANGE,
            FString::Printf(TEXT("tile (%d,%d) is outside the georeference's %d x %d grid"),
                CanvasTile.Col, CanvasTile.Row, Geo.Grid.Cols, Geo.Grid.Rows));
        return true;
    }

    // The whole world -> canvas mapping, in one place. The mosaic pixel comes from the
    // georeference; the canvas is that pixel space shifted by the tile's own origin - which is why
    // a coordinate annotated onto a tile lands at the same ground as on the full image.
    const FVector2D CanvasOrigin = bTileMode
        ? TilePixelToPixel(Geo.Grid, CanvasTile, FVector2D::ZeroVector)
        : FVector2D::ZeroVector;
    auto ToCanvas = [&Geo, &CanvasOrigin](const FVector& World) -> FVector2D
    {
        return WorldToPixel(Geo.Grid, World) - CanvasOrigin;
    };

    // ---- snapping ----
    const TSharedPtr<FJsonObject>& Payload = Ctx.GetRawPayload();
    const bool bSnapSupplied = Payload.IsValid() && Payload->HasField(TEXT("snapCm"));
    const double SnapCm = Ctx.GetNumber(TEXT("snapCm"), 0.0);
    if (bSnapSupplied && !(SnapCm > 0.0))
    {
        // SnapToWorldGrid passes a non-positive grid size straight through untouched
        // (TileGridUtils.h), so an unvalidated 0 here would look like a snap that quietly did
        // nothing. A verb taking a grid size from a caller has to reject it.
        Ctx.SendError(ErrorCodes::ERR_INVALID_PARAMS,
            FString::Printf(TEXT("'snapCm' must be greater than 0 when supplied, got %f. A non-positive grid size is a no-op snap, which would report snapped coordinates that were never snapped."),
                SnapCm));
        return true;
    }
    const bool bSnap = SnapCm > 0.0;
    auto MaybeSnap = [bSnap, SnapCm](const FVector& World) -> FVector
    {
        return bSnap ? SnapToWorldGrid(World, SnapCm) : World;
    };

    // ---- output target, checked before any drawing ----
    FString DefaultName = SanitizeBaseName(FPaths::GetBaseFilename(SourcePath), TEXT("image"));
    DefaultName += bTileMode
        ? FString::Printf(TEXT("_r%dc%d_annotated"), CanvasTile.Row, CanvasTile.Col)
        : FString(TEXT("_annotated"));
    const FString OutName = SanitizeBaseName(Ctx.GetString(TEXT("outputName")), DefaultName);
    const FString OutputDir = ResolveOutputDir(Ctx.GetString(TEXT("outputDir")), TEXT("annotated"));
    const FString OutputPath = OutputDir / (OutName + TEXT(".png"));
    if (!Ctx.GetBool(TEXT("overwrite"), false) && IFileManager::Get().FileExists(*OutputPath))
    {
        Ctx.SendError(ErrorCodes::ERR_ALREADY_EXISTS,
            FString::Printf(TEXT("%s already exists and overwrite is false. Nothing was drawn."), *OutputPath));
        return true;
    }

    // ---- collect the requested marks ----
    const TSharedPtr<FJsonObject> GridSpec = Ctx.GetObject(TEXT("grid"));
    const TArray<TSharedPtr<FJsonValue>>* Crosshairs = Ctx.GetArray(TEXT("crosshairs"));
    const TArray<TSharedPtr<FJsonValue>>* Boxes = Ctx.GetArray(TEXT("boxes"));
    const TArray<TSharedPtr<FJsonValue>>* Labels = Ctx.GetArray(TEXT("labels"));
    const int32 MarkCount =
        (Crosshairs ? Crosshairs->Num() : 0) + (Boxes ? Boxes->Num() : 0) + (Labels ? Labels->Num() : 0);
    if (!GridSpec.IsValid() && MarkCount == 0)
    {
        // An empty mark set is an error, not a zero-item success: otherwise a typo in an overlay
        // key produces a byte-identical copy of the input, reported as an annotation.
        Ctx.SendError(ErrorCodes::ERR_NOTHING_TO_ANNOTATE,
            TEXT("Nothing to draw: supply at least one of 'grid', 'crosshairs', 'boxes', 'labels'. "
                 "A copy of the input with no marks on it is not an annotation."));
        return true;
    }

    FBitmap Canvas = Original;
    FSurface Surface = Canvas.Surface();
    TSharedPtr<FJsonObject> Overlays = MakeShared<FJsonObject>();
    int32 MarksDrawn = 0;

    // Draws one mark, then reports whether the pixels under it actually changed.
    auto PaintAndMeasure = [&Canvas](int32 MinX, int32 MinY, int32 MaxX, int32 MaxY,
        TFunctionRef<void()> Paint) -> bool
    {
        FIntRect Clipped;
        if (!PwAnnotateClipRect(MinX, MinY, MaxX, MaxY, Canvas.Width, Canvas.Height, Clipped))
        {
            return false;
        }
        const uint64 Before = PwAnnotateHashRegion(Canvas, Clipped);
        Paint();
        return PwAnnotateHashRegion(Canvas, Clipped) != Before;
    };

    // ---- grid ----
    if (GridSpec.IsValid())
    {
        const TArray<FString> Allowed = {
            TEXT("spacingCm"), TEXT("color"), TEXT("opacity"), TEXT("thicknessPx"), TEXT("labels") };
        FString KeyErr;
        if (!RejectUnknownKeys(GridSpec, Allowed, TEXT("grid"), KeyErr))
        {
            Ctx.SendError(ErrorCodes::ERR_INVALID_PARAMS, KeyErr);
            return true;
        }
        double SpacingCm = 0.0;
        if (!GridSpec->TryGetNumberField(TEXT("spacingCm"), SpacingCm) || SpacingCm <= 0.0)
        {
            Ctx.SendError(ErrorCodes::ERR_INVALID_PARAMS,
                TEXT("'grid.spacingCm' is required and must be greater than 0 (world centimetres between lines)"));
            return true;
        }
        FPwAnnotateStyle Style;
        FString StyleErr;
        if (!PwAnnotateReadStyle(GridSpec, TEXT("grid"), TEXT("white"), 0.25f, 1, Style, StyleErr))
        {
            Ctx.SendError(ErrorCodes::ERR_INVALID_PARAMS, StyleErr);
            return true;
        }
        bool bGridLabels = true;
        GridSpec->TryGetBoolField(TEXT("labels"), bGridLabels);

        // The world span this canvas actually shows.
        FWorldExtent2D Visible = Geo.Grid.World;
        if (bTileMode && !TileWorldExtent(Geo.Grid, CanvasTile, Visible))
        {
            Ctx.SendError(ErrorCodes::ERR_TILE_OUT_OF_RANGE,
                FString::Printf(TEXT("tile (%d,%d) has no world extent"), CanvasTile.Col, CanvasTile.Row));
            return true;
        }

        // COUNT IN DOUBLE, CHECK IN DOUBLE, NARROW LAST. Narrowing first is a guard that fails
        // open: span/spacing for a very small spacingCm exceeds INT32_MAX, the narrowing wraps
        // negative, `> MaxGridLinesPerAxis` is then false, and the verb answered SUCCESS with
        // linesAcross: 0 - a refusal turned into a silent no-op (rpc-design.md §1). NaN and
        // infinity land here too: a comparison against NaN is false either way round, so the
        // ceiling test is written as a negated `<=` and an unorderable count refuses.
        const double FirstAcross = FMath::CeilToDouble(Visible.Min.X / SpacingCm) * SpacingCm;
        const double FirstDown = FMath::CeilToDouble(Visible.Min.Y / SpacingCm) * SpacingCm;
        const double CountAcrossExact =
            FMath::FloorToDouble((Visible.Max.X - FirstAcross) / SpacingCm) + 1.0;
        const double CountDownExact =
            FMath::FloorToDouble((Visible.Max.Y - FirstDown) / SpacingCm) + 1.0;
        const double LineCeiling = static_cast<double>(MaxGridLinesPerAxis);
        if (!(CountAcrossExact <= LineCeiling) || !(CountDownExact <= LineCeiling))
        {
            // %d cannot print these: the whole point is that the count does not fit an int32. A
            // count past 2^53 is printed as a magnitude because a double no longer represents
            // consecutive integers there, and printing every digit would assert a precision the
            // value does not carry. spacingCm is printed with %g rather than rounded to an int,
            // which turned the denormal spacing that produces this into "0 cm".
            auto DescribeLineCount = [](double Count) -> FString
            {
                if (FMath::IsNaN(Count))
                {
                    return TEXT("an undefined number of");
                }
                if (!FMath::IsFinite(Count))
                {
                    return TEXT("infinitely many");
                }
                const double Clamped = FMath::Max(Count, 0.0);
                return Clamped >= 9007199254740992.0
                    ? FString::Printf(TEXT("~%.3g"), Clamped)
                    : FString::Printf(TEXT("%.0f"), Clamped);
            };
            Ctx.SendError(ErrorCodes::ERR_GRID_TOO_DENSE,
                FString::Printf(TEXT("%g cm spacing needs %s x %s lines across a %.1f x %.1f cm frame; the ceiling is %d per axis. Past about one line per pixel the overlay IS the image."),
                    SpacingCm, *DescribeLineCount(CountAcrossExact), *DescribeLineCount(CountDownExact),
                    Visible.SpanAcross(), Visible.SpanDown(), MaxGridLinesPerAxis));
            return true;
        }
        // Only now is the narrowing safe: the ceiling refused everything above it, and the clamp
        // floors a degenerate extent (a span shorter than one spacing, or a -infinity count) at 0.
        const int32 CountAcross = static_cast<int32>(FMath::Clamp(CountAcrossExact, 0.0, LineCeiling));
        const int32 CountDown = static_cast<int32>(FMath::Clamp(CountDownExact, 0.0, LineCeiling));

        const FPaint LinePaint(Style.Color, Style.Opacity);
        FLabelStyle LabelStyle;
        LabelStyle.Anchor = ELabelAnchor::TextTopLeft;

        int32 LinesAcross = 0;
        int32 LinesDown = 0;
        int32 LabelsDrawn = 0;
        int32 NextLabelX = 0;
        int32 NextLabelY = 0;

        const uint64 GridBefore = PwAnnotateHashRegion(Canvas,
            FIntRect(0, 0, Canvas.Width, Canvas.Height));

        for (int32 Index = 0; Index < FMath::Max(CountAcross, 0); ++Index)
        {
            const double Coord = FirstAcross + Index * SpacingCm;
            FVector World = FVector::ZeroVector;
            SetAxisValue(World, Geo.Grid.Axes.ScreenXAxis, Coord);
            SetAxisValue(World, Geo.Grid.Axes.ScreenYAxis, Visible.Centre().Y);
            SetAxisValue(World, Geo.Grid.Axes.DepthAxis(), Geo.DepthCm);
            const int32 PixelX = FMath::RoundToInt32(ToCanvas(World).X);
            if (PixelX < 0 || PixelX >= Canvas.Width)
            {
                continue;
            }
            DrawLine(Surface, PixelX, 0, PixelX, Canvas.Height - 1, Style.ThicknessPx, LinePaint);
            ++LinesAcross;

            if (bGridLabels)
            {
                // Labels ride the TOP edge, never the field - a label in the middle of the frame
                // hides exactly the content the grid exists to let a reader locate.
                const FString Text = FString::Printf(TEXT("%s %d"),
                    AxisName(Geo.Grid.Axes.ScreenXAxis), FMath::RoundToInt(Coord));
                const FIntRect Plate = MeasureLabelPlate(Text, PixelX + 2, 2, LabelStyle);
                if (Plate.Min.X >= NextLabelX && Plate.Max.X <= Canvas.Width)
                {
                    DrawLabel(Surface, Text, PixelX + 2, 2, LabelStyle);
                    NextLabelX = Plate.Max.X + 4;
                    ++LabelsDrawn;
                }
            }
        }

        for (int32 Index = 0; Index < FMath::Max(CountDown, 0); ++Index)
        {
            const double Coord = FirstDown + Index * SpacingCm;
            FVector World = FVector::ZeroVector;
            SetAxisValue(World, Geo.Grid.Axes.ScreenXAxis, Visible.Centre().X);
            SetAxisValue(World, Geo.Grid.Axes.ScreenYAxis, Coord);
            SetAxisValue(World, Geo.Grid.Axes.DepthAxis(), Geo.DepthCm);
            const int32 PixelY = FMath::RoundToInt32(ToCanvas(World).Y);
            if (PixelY < 0 || PixelY >= Canvas.Height)
            {
                continue;
            }
            DrawLine(Surface, 0, PixelY, Canvas.Width - 1, PixelY, Style.ThicknessPx, LinePaint);
            ++LinesDown;

            if (bGridLabels)
            {
                const FString Text = FString::Printf(TEXT("%s %d"),
                    AxisName(Geo.Grid.Axes.ScreenYAxis), FMath::RoundToInt(Coord));
                const FIntRect Plate = MeasureLabelPlate(Text, 2, PixelY + 2, LabelStyle);
                if (Plate.Min.Y >= NextLabelY && Plate.Max.Y <= Canvas.Height)
                {
                    DrawLabel(Surface, Text, 2, PixelY + 2, LabelStyle);
                    NextLabelY = Plate.Max.Y + 4;
                    ++LabelsDrawn;
                }
            }
        }

        const bool bGridChangedPixels =
            PwAnnotateHashRegion(Canvas, FIntRect(0, 0, Canvas.Width, Canvas.Height)) != GridBefore;

        TSharedPtr<FJsonObject> GridEcho = MakeShared<FJsonObject>();
        GridEcho->SetNumberField(TEXT("spacingCm"), SpacingCm);
        GridEcho->SetNumberField(TEXT("opacity"), Style.Opacity);
        GridEcho->SetNumberField(TEXT("thicknessPx"), Style.ThicknessPx);
        GridEcho->SetNumberField(TEXT("linesAcross"), LinesAcross);
        GridEcho->SetNumberField(TEXT("linesDown"), LinesDown);
        GridEcho->SetNumberField(TEXT("labelsDrawn"), LabelsDrawn);
        GridEcho->SetBoolField(TEXT("drawn"), bGridChangedPixels);
        Overlays->SetObjectField(TEXT("grid"), GridEcho);
        if (bGridChangedPixels)
        {
            ++MarksDrawn;
        }
    }

    // ---- crosshairs ----
    if (Crosshairs)
    {
        TArray<TSharedPtr<FJsonValue>> Echo;
        for (int32 Index = 0; Index < Crosshairs->Num(); ++Index)
        {
            const TSharedPtr<FJsonObject>* EntryPtr = nullptr;
            if (!(*Crosshairs)[Index].IsValid() || !(*Crosshairs)[Index]->TryGetObject(EntryPtr) || !EntryPtr)
            {
                Ctx.SendError(ErrorCodes::ERR_INVALID_PARAMS,
                    FString::Printf(TEXT("crosshairs[%d] is not an object"), Index));
                return true;
            }
            const TSharedPtr<FJsonObject>& Entry = *EntryPtr;
            const FString Context = FString::Printf(TEXT("crosshairs[%d]"), Index);
            const TArray<FString> Allowed = {
                TEXT("x"), TEXT("y"), TEXT("z"), TEXT("label"), TEXT("color"), TEXT("opacity"),
                TEXT("sizePx"), TEXT("thicknessPx") };
            FString EntryErr;
            if (!RejectUnknownKeys(Entry, Allowed, Context, EntryErr))
            {
                Ctx.SendError(ErrorCodes::ERR_INVALID_PARAMS, EntryErr);
                return true;
            }
            FVector Requested = FVector::ZeroVector;
            if (!PwAnnotateReadPoint(Entry, Context, Requested, EntryErr))
            {
                Ctx.SendError(ErrorCodes::ERR_INVALID_PARAMS, EntryErr);
                return true;
            }
            FPwAnnotateStyle Style;
            if (!PwAnnotateReadStyle(Entry, Context, TEXT("red"), 1.0f, 2, Style, EntryErr))
            {
                Ctx.SendError(ErrorCodes::ERR_INVALID_PARAMS, EntryErr);
                return true;
            }
            int32 SizePx = 24;
            Entry->TryGetNumberField(TEXT("sizePx"), SizePx);
            if (SizePx < 2 || SizePx > 4096)
            {
                Ctx.SendError(ErrorCodes::ERR_INVALID_PARAMS,
                    FString::Printf(TEXT("%s: 'sizePx' must be between 2 and 4096, got %d"), *Context, SizePx));
                return true;
            }
            FString LabelText;
            Entry->TryGetStringField(TEXT("label"), LabelText);

            const FVector Used = MaybeSnap(Requested);
            const FVector2D Pixel = ToCanvas(Used);
            const int32 PX = FMath::RoundToInt32(Pixel.X);
            const int32 PY = FMath::RoundToInt32(Pixel.Y);
            const int32 Half = SizePx / 2;

            // The label plate takes the mark's COLOUR but not its opacity, deliberately: `opacity`
            // governs the geometry, which is drawn translucent so it does not bury what it points
            // at, and text at that alpha over arbitrary imagery is unreadable - an unreadable
            // coordinate is the whole mark wasted. Same rule as the grid's edge labels. A mark
            // whose text must fade with it is the `labels[]` entry, where opacity IS the mark.
            FLabelStyle LabelStyle;
            LabelStyle.Ink = PinWrightBitmapPaint::FPaint(Style.Color);
            const FIntRect Plate = LabelText.IsEmpty()
                ? FIntRect(PX, PY, PX, PY)
                : MeasureLabelPlate(LabelText, PX + Half + 3, PY - Half, LabelStyle);

            const int32 MinX = FMath::Min(PX - Half - Style.ThicknessPx, Plate.Min.X);
            const int32 MinY = FMath::Min(PY - Half - Style.ThicknessPx, Plate.Min.Y);
            const int32 MaxX = FMath::Max(PX + Half + Style.ThicknessPx, Plate.Max.X);
            const int32 MaxY = FMath::Max(PY + Half + Style.ThicknessPx, Plate.Max.Y);

            const FPaint Paint(Style.Color, Style.Opacity);
            const bool bDrawn = PaintAndMeasure(MinX, MinY, MaxX, MaxY, [&]()
            {
                DrawLine(Surface, PX - Half, PY, PX + Half, PY, Style.ThicknessPx, Paint);
                DrawLine(Surface, PX, PY - Half, PX, PY + Half, Style.ThicknessPx, Paint);
                if (!LabelText.IsEmpty())
                {
                    DrawLabel(Surface, LabelText, PX + Half + 3, PY - Half, LabelStyle);
                }
            });
            MarksDrawn += bDrawn ? 1 : 0;

            TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
            Row->SetNumberField(TEXT("index"), Index);
            Row->SetObjectField(TEXT("world"), MakeVectorObject(Requested));
            if (bSnap)
            {
                Row->SetObjectField(TEXT("worldSnapped"), MakeVectorObject(Used));
            }
            Row->SetObjectField(TEXT("pixel"), MakePixelObject(Pixel));
            Row->SetObjectField(TEXT("mosaicPixel"), MakePixelObject(Pixel + CanvasOrigin));
            Row->SetBoolField(TEXT("inFrame"),
                Pixel.X >= 0.0 && Pixel.Y >= 0.0 &&
                Pixel.X <= static_cast<double>(Canvas.Width) && Pixel.Y <= static_cast<double>(Canvas.Height));
            Row->SetBoolField(TEXT("drawn"), bDrawn);
            if (!LabelText.IsEmpty())
            {
                Row->SetStringField(TEXT("label"), LabelText);
            }
            Echo.Add(MakeShared<FJsonValueObject>(Row));
        }
        Overlays->SetArrayField(TEXT("crosshairs"), Echo);
    }

    // ---- boxes ----
    if (Boxes)
    {
        TArray<TSharedPtr<FJsonValue>> Echo;
        for (int32 Index = 0; Index < Boxes->Num(); ++Index)
        {
            const TSharedPtr<FJsonObject>* EntryPtr = nullptr;
            if (!(*Boxes)[Index].IsValid() || !(*Boxes)[Index]->TryGetObject(EntryPtr) || !EntryPtr)
            {
                Ctx.SendError(ErrorCodes::ERR_INVALID_PARAMS,
                    FString::Printf(TEXT("boxes[%d] is not an object"), Index));
                return true;
            }
            const TSharedPtr<FJsonObject>& Entry = *EntryPtr;
            const FString Context = FString::Printf(TEXT("boxes[%d]"), Index);
            const TArray<FString> Allowed = {
                TEXT("min"), TEXT("max"), TEXT("label"), TEXT("color"), TEXT("opacity"), TEXT("thicknessPx") };
            FString EntryErr;
            if (!RejectUnknownKeys(Entry, Allowed, Context, EntryErr))
            {
                Ctx.SendError(ErrorCodes::ERR_INVALID_PARAMS, EntryErr);
                return true;
            }
            FVector WorldMin = FVector::ZeroVector;
            FVector WorldMax = FVector::ZeroVector;
            if (!ParseWorldPointField(Entry, TEXT("min"), WorldMin, EntryErr) ||
                !ParseWorldPointField(Entry, TEXT("max"), WorldMax, EntryErr))
            {
                Ctx.SendError(ErrorCodes::ERR_INVALID_PARAMS,
                    FString::Printf(TEXT("%s: %s"), *Context, *EntryErr));
                return true;
            }
            FPwAnnotateStyle Style;
            if (!PwAnnotateReadStyle(Entry, Context, TEXT("cyan"), 1.0f, 2, Style, EntryErr))
            {
                Ctx.SendError(ErrorCodes::ERR_INVALID_PARAMS, EntryErr);
                return true;
            }
            FString LabelText;
            Entry->TryGetStringField(TEXT("label"), LabelText);

            const FVector UsedMin = MaybeSnap(WorldMin);
            const FVector UsedMax = MaybeSnap(WorldMax);
            const FVector2D A = ToCanvas(UsedMin);
            const FVector2D B = ToCanvas(UsedMax);
            const int32 X0 = FMath::RoundToInt32(FMath::Min(A.X, B.X));
            const int32 Y0 = FMath::RoundToInt32(FMath::Min(A.Y, B.Y));
            const int32 X1 = FMath::RoundToInt32(FMath::Max(A.X, B.X));
            const int32 Y1 = FMath::RoundToInt32(FMath::Max(A.Y, B.Y));

            // Opaque label plate over a translucent stroke - see the crosshair note above.
            FLabelStyle LabelStyle;
            LabelStyle.Ink = PinWrightBitmapPaint::FPaint(Style.Color);
            const FIntRect Plate = LabelText.IsEmpty()
                ? FIntRect(X0, Y0, X0, Y0)
                : MeasureLabelPlate(LabelText, X0, Y0 - MeasureLabelTextHeight(LabelStyle.Scale) - 6, LabelStyle);

            const FPaint Paint(Style.Color, Style.Opacity);
            const bool bDrawn = PaintAndMeasure(
                FMath::Min(X0, Plate.Min.X), FMath::Min(Y0, Plate.Min.Y),
                FMath::Max(X1, Plate.Max.X), FMath::Max(Y1, Plate.Max.Y), [&]()
            {
                StrokeBox(Surface, X0, Y0, X1, Y1, Style.ThicknessPx, Paint);
                if (!LabelText.IsEmpty())
                {
                    DrawLabel(Surface, LabelText, X0,
                        Y0 - MeasureLabelTextHeight(LabelStyle.Scale) - 6, LabelStyle);
                }
            });
            MarksDrawn += bDrawn ? 1 : 0;

            TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
            Row->SetNumberField(TEXT("index"), Index);
            Row->SetObjectField(TEXT("worldMin"), MakeVectorObject(WorldMin));
            Row->SetObjectField(TEXT("worldMax"), MakeVectorObject(WorldMax));
            if (bSnap)
            {
                Row->SetObjectField(TEXT("worldMinSnapped"), MakeVectorObject(UsedMin));
                Row->SetObjectField(TEXT("worldMaxSnapped"), MakeVectorObject(UsedMax));
            }
            Row->SetObjectField(TEXT("pixelMin"), MakePixelObject(FVector2D(X0, Y0)));
            Row->SetObjectField(TEXT("pixelMax"), MakePixelObject(FVector2D(X1, Y1)));
            Row->SetObjectField(TEXT("mosaicPixelMin"), MakePixelObject(FVector2D(X0, Y0) + CanvasOrigin));
            Row->SetObjectField(TEXT("mosaicPixelMax"), MakePixelObject(FVector2D(X1, Y1) + CanvasOrigin));
            Row->SetBoolField(TEXT("inFrame"),
                X1 >= 0 && Y1 >= 0 && X0 < Canvas.Width && Y0 < Canvas.Height);
            Row->SetBoolField(TEXT("drawn"), bDrawn);
            if (!LabelText.IsEmpty())
            {
                Row->SetStringField(TEXT("label"), LabelText);
            }
            Echo.Add(MakeShared<FJsonValueObject>(Row));
        }
        Overlays->SetArrayField(TEXT("boxes"), Echo);
    }

    // ---- free labels ----
    if (Labels)
    {
        TArray<TSharedPtr<FJsonValue>> Echo;
        for (int32 Index = 0; Index < Labels->Num(); ++Index)
        {
            const TSharedPtr<FJsonObject>* EntryPtr = nullptr;
            if (!(*Labels)[Index].IsValid() || !(*Labels)[Index]->TryGetObject(EntryPtr) || !EntryPtr)
            {
                Ctx.SendError(ErrorCodes::ERR_INVALID_PARAMS,
                    FString::Printf(TEXT("labels[%d] is not an object"), Index));
                return true;
            }
            const TSharedPtr<FJsonObject>& Entry = *EntryPtr;
            const FString Context = FString::Printf(TEXT("labels[%d]"), Index);
            const TArray<FString> Allowed = {
                TEXT("x"), TEXT("y"), TEXT("z"), TEXT("text"), TEXT("color"), TEXT("opacity"), TEXT("scale") };
            FString EntryErr;
            if (!RejectUnknownKeys(Entry, Allowed, Context, EntryErr))
            {
                Ctx.SendError(ErrorCodes::ERR_INVALID_PARAMS, EntryErr);
                return true;
            }
            FVector Requested = FVector::ZeroVector;
            if (!PwAnnotateReadPoint(Entry, Context, Requested, EntryErr))
            {
                Ctx.SendError(ErrorCodes::ERR_INVALID_PARAMS, EntryErr);
                return true;
            }
            FString Text;
            if (!Entry->TryGetStringField(TEXT("text"), Text) || Text.IsEmpty())
            {
                Ctx.SendError(ErrorCodes::ERR_INVALID_PARAMS,
                    FString::Printf(TEXT("%s: 'text' is required and must be a non-empty string"), *Context));
                return true;
            }
            FPwAnnotateStyle Style;
            if (!PwAnnotateReadStyle(Entry, Context, TEXT("yellow"), 1.0f, 1, Style, EntryErr))
            {
                Ctx.SendError(ErrorCodes::ERR_INVALID_PARAMS, EntryErr);
                return true;
            }
            int32 Scale = PinWrightBitmapPaint::DefaultLabelScale;
            Entry->TryGetNumberField(TEXT("scale"), Scale);
            if (Scale < 1 || Scale > 16)
            {
                Ctx.SendError(ErrorCodes::ERR_INVALID_PARAMS,
                    FString::Printf(TEXT("%s: 'scale' must be between 1 and 16, got %d"), *Context, Scale));
                return true;
            }

            const FVector Used = MaybeSnap(Requested);
            const FVector2D Pixel = ToCanvas(Used);
            const int32 PX = FMath::RoundToInt32(Pixel.X);
            const int32 PY = FMath::RoundToInt32(Pixel.Y);

            FLabelStyle LabelStyle;
            LabelStyle.Scale = Scale;
            LabelStyle.Ink = PinWrightBitmapPaint::FPaint(Style.Color, Style.Opacity);
            const FIntRect Plate = MeasureLabelPlate(Text, PX, PY, LabelStyle);

            const bool bDrawn = PaintAndMeasure(Plate.Min.X, Plate.Min.Y, Plate.Max.X - 1, Plate.Max.Y - 1,
                [&]() { DrawLabel(Surface, Text, PX, PY, LabelStyle); });
            MarksDrawn += bDrawn ? 1 : 0;

            TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
            Row->SetNumberField(TEXT("index"), Index);
            Row->SetStringField(TEXT("text"), Text);
            Row->SetObjectField(TEXT("world"), MakeVectorObject(Requested));
            if (bSnap)
            {
                Row->SetObjectField(TEXT("worldSnapped"), MakeVectorObject(Used));
            }
            Row->SetObjectField(TEXT("pixel"), MakePixelObject(Pixel));
            Row->SetObjectField(TEXT("mosaicPixel"), MakePixelObject(Pixel + CanvasOrigin));
            Row->SetBoolField(TEXT("inFrame"),
                Pixel.X >= 0.0 && Pixel.Y >= 0.0 &&
                Pixel.X <= static_cast<double>(Canvas.Width) && Pixel.Y <= static_cast<double>(Canvas.Height));
            Row->SetBoolField(TEXT("drawn"), bDrawn);
            Echo.Add(MakeShared<FJsonValueObject>(Row));
        }
        Overlays->SetArrayField(TEXT("labels"), Echo);
    }

    // ---- how much of the image actually changed ----
    int64 PixelsChanged = 0;
    for (int32 Index = 0; Index < Canvas.Pixels.Num(); ++Index)
    {
        if (Canvas.Pixels[Index] != Original.Pixels[Index])
        {
            ++PixelsChanged;
        }
    }

    if (!SaveBitmapPng(OutputPath, Canvas, ErrCode, ErrMsg))
    {
        Ctx.SendError(ErrCode, ErrMsg);
        return true;
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("source"), SourcePath);
    Result->SetStringField(TEXT("output"), OutputPath);
    Result->SetNumberField(TEXT("width"), Canvas.Width);
    Result->SetNumberField(TEXT("height"), Canvas.Height);
    Result->SetBoolField(TEXT("tileMode"), bTileMode);
    if (bTileMode)
    {
        TSharedPtr<FJsonObject> TileObj = MakeShared<FJsonObject>();
        TileObj->SetNumberField(TEXT("col"), CanvasTile.Col);
        TileObj->SetNumberField(TEXT("row"), CanvasTile.Row);
        Result->SetObjectField(TEXT("tile"), TileObj);
    }
    Result->SetObjectField(TEXT("canvasOriginInMosaic"), MakePixelObject(CanvasOrigin));
    Result->SetObjectField(TEXT("georeference"), SerializeGeoreference(Geo));
    Result->SetObjectField(TEXT("grid"), DescribeGrid(Geo));
    if (bSnap)
    {
        Result->SetNumberField(TEXT("snapCm"), SnapCm);
    }
    Result->SetObjectField(TEXT("overlays"), Overlays);
    Result->SetNumberField(TEXT("marksRequested"), MarkCount + (GridSpec.IsValid() ? 1 : 0));
    Result->SetNumberField(TEXT("marksDrawn"), MarksDrawn);
    Result->SetNumberField(TEXT("pixelsChanged"), static_cast<double>(PixelsChanged));

    // Non-square pixels are legal but are usually a georeference mistake, and in tile mode they
    // are the one available tell that the supplied image is not the tile it was declared to be
    // (passing the whole mosaic with `tile` set multiplies the grid's pixel size by cols/rows).
    // Reported, not refused: an anamorphic reference is rare but real, and every coordinate read
    // off this annotation inherits the scale either way.
    if (!Geo.Grid.HasSquarePixels())
    {
        TArray<TSharedPtr<FJsonValue>> Warnings;
        Warnings.Add(MakeShared<FJsonValueString>(FString::Printf(
            TEXT("Non-square pixels: %.4f cm/px across (%s) vs %.4f cm/px down (%s).%s"),
            Geo.Grid.WorldUnitsPerPixelAcross(), AxisName(Geo.Grid.Axes.ScreenXAxis),
            Geo.Grid.WorldUnitsPerPixelDown(), AxisName(Geo.Grid.Axes.ScreenYAxis),
            bTileMode
                ? TEXT(" 'tile' was set, so this image was treated as ONE tile of the grid - check it is the tile and not the whole mosaic.")
                : TEXT(" Check worldMin/worldMax against the image dimensions."))));
        Result->SetArrayField(TEXT("warnings"), Warnings);
    }

    Ctx.SendSuccess(FString::Printf(TEXT("Annotated %s -> %s (%d of %d marks changed pixels, %lld pixels differ)"),
        *FPaths::GetCleanFilename(SourcePath), *OutputPath, MarksDrawn,
        MarkCount + (GridSpec.IsValid() ? 1 : 0), PixelsChanged), Result);
    return true;
}
