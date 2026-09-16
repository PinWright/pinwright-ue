// Copyright (c) 2026 Alexander Penkin. MIT License.

// render.capture_ortho_tiles - render the open level orthographically over a caller-specified
// world extent, split into an N x M grid of tiles at a fixed per-tile pixel resolution, writing
// one PNG per tile plus a manifest carrying each tile's world extent, pixel origin and standalone
// georeference.
//
// THE POINT OF THE VERB, in one sentence: a reference image cut by image.tile on the same
// georeference lines up tile-for-tile with what this rendered, at whatever resolution either side
// happens to be.
//
// The verb owns no maths and no georeference vocabulary of its own. Every world <-> pixel answer
// comes from PinWrightTileGrid, every wire georeference is built by the same
// PinWrightImage::SerializeGeoreference that image.tile emits and image.annotate / image.compare
// parse, and every PNG goes out through PinWrightImage::SaveBitmapPng. That is deliberate: two
// implementations of the tiling maths would let the capture place cameras by one convention and
// the reader measure pixels by another, and the disagreement is invisible until a coordinate is
// ~1000 uu wrong.
//
// The renderer is a transient USceneCaptureComponent2D (PinWrightOrthoTiles), never the editor
// viewport - see OrthoTileCaptureUtils.h for the three measured reasons and for the near-plane-0
// trap that decides where the camera goes. Two consequences the caller sees here: nothing this
// verb does can dirty the level (measured and reported, not asserted), and the response records
// the renderer so a later comparison can refuse a cross-path pair - viewport and scene capture
// differ by 5.76% mean absolute error against a 1.30% viewport self-noise floor, and an ideal
// fitted tone LUT only closes it to 3.28%.

#include "Handlers/HandlerContext.h"
#include "Handlers/HandlerRegistration.h"
#include "Handlers/ParamSpec.h"
#include "Handlers/ErrorCodes.h"
#include "Handlers/Image/ImageOps.h"
#include "Handlers/Render/CaptureRendererNames.h"
#include "Handlers/Render/OrthoTileCaptureUtils.h"
#include "Handlers/Render/PreviewViewportCaptureUtils.h"
#include "Handlers/Render/TileGridUtils.h"
#include "Utils/ScreenshotUtils.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/Package.h"

// No file-scope `using namespace` anywhere in this file: Unity merges translation units, so a
// using-directive outside a function body leaks into every .cpp compiled after it in the same
// blob. The handler body below takes its using-declarations at function scope, which does not.
namespace
{
    // Above this the per-tile array is written to the manifest only, matching image.tile's
    // threshold so the two verbs' responses are the same size for the same grid.
    constexpr int32 PwOrthoMaxTilesInResponse = 64;

    constexpr int32 PwOrthoDefaultTilePixels = PinWrightRenderCapture::DefaultCaptureEdge;

    FString PwOrthoPad(int32 Value, int32 Digits)
    {
        FString Text = FString::FromInt(Value);
        while (Text.Len() < Digits)
        {
            Text = TEXT("0") + Text;
        }
        return Text;
    }

    // Identical to image.tile's naming, so a capture set and a reference set cut on the same
    // georeference can be paired file-for-file by name alone.
    FString PwOrthoTileFileName(const FString& Prefix, int32 Col, int32 Row, int32 ColDigits, int32 RowDigits)
    {
        return FString::Printf(TEXT("%s_r%sc%s.png"), *Prefix,
            *PwOrthoPad(Row, RowDigits), *PwOrthoPad(Col, ColDigits));
    }

    // The georeference of ONE tile: the same axes and depth as the parent, the tile's own world
    // AABB, and a 1x1 subdivision. Built from PinWrightTileGrid::TileWorldExtent so it is the
    // georeference's own answer rather than a second derivation of it - the seam a reader
    // measures across is whatever TileGridUtils says it is.
    bool PwOrthoMakeTileGeoreference(const PinWrightImage::FGeoreference& Parent,
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

    TSharedPtr<FJsonObject> PwOrthoMakeRotatorObject(const FRotator& R)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetNumberField(TEXT("pitch"), R.Pitch);
        Obj->SetNumberField(TEXT("yaw"), R.Yaw);
        Obj->SetNumberField(TEXT("roll"), R.Roll);
        return Obj;
    }

    // The dirty state of every package backing the editor world's levels, plus the world's own
    // outermost. Sampled before and after the burst so "the capture did not dirty the level" is a
    // measurement rather than a claim about a mechanism.
    int32 PwOrthoCountDirtyLevelPackages(UWorld* World)
    {
        if (!World)
        {
            return -1;
        }
        TSet<const UPackage*> Seen;
        int32 Dirty = 0;
        if (UPackage* Outer = World->GetOutermost())
        {
            Seen.Add(Outer);
            Dirty += Outer->IsDirty() ? 1 : 0;
        }
        for (const ULevel* Level : World->GetLevels())
        {
            if (!Level)
            {
                continue;
            }
            const UPackage* Package = Level->GetOutermost();
            if (!Package || Seen.Contains(Package))
            {
                continue;
            }
            Seen.Add(Package);
            Dirty += Package->IsDirty() ? 1 : 0;
        }
        return Dirty;
    }
}

REGISTER_RPC_HANDLER("render.capture_ortho_tiles", "render",
    "Render the open level orthographically over a world extent, as an N x M grid of georeferenced PNG tiles at a fixed per-tile resolution, plus a manifest that image.tile / image.annotate / image.compare read back verbatim.",
    RPC_PARAMS(
        RPC_PARAM_REQ("worldMin", "object",
            "One corner of the world box to cover, {x, y, z} in centimetres. The two in-plane components (per `axes`) define the extent; the depth-axis components of worldMin/worldMax define the MEASUREMENT plane a reader's pixel -> world answers land on, not where the camera goes (see cameraDepthCm)."),
        RPC_PARAM_REQ("worldMax", "object",
            "The opposite corner, {x, y, z} in centimetres. Components may be given in either order."),
        RPC_PARAM_REQ("axes", "string",
            "Which world axis lands on screen X and which on screen Y: a preset name (top_down_x_up_y_right | top_down_x_right_y_down | front_y_right_z_up | side_x_right_z_up) or the explicit object {screenXAxis, screenXPositive, screenYAxis, screenYPositive}. REQUIRED, with no default: the two top-down presets are both correct and produce transposed images, so a default would silently mirror every coordinate read off the tiles. The camera rotation is derived from this and reported back."),
        RPC_PARAM_REQ("exposure", "object|number",
            "REQUIRED here, unlike on the other capture verbs. Pass a bare number as shorthand for {mode:\"fixed\", ev100:<number>} (range -30..30), or {mode:\"auto\"} to opt out explicitly. A scene capture keeps no persistent view state, so with auto-exposure EVERY TILE exposes to its own content and the mosaic shows a step at every seam - there is no safe default, so the choice is asked for. The response's `exposure.pinned` is measured, not echoed."),
        RPC_PARAM_OPT("cmPerPixel", "number",
            "Target world centimetres per pixel. The grid is sized to cover the requested extent at no coarser than this, then the extent is EXPANDED symmetrically about its centre so it is exactly cols x rows whole tiles - reported as `extentExpanded`. Exactly one of cmPerPixel / (cols + rows)."),
        RPC_PARAM_OPT("cols", "number",
            "Explicit column count, subdividing the requested extent exactly as given (no expansion). Requires `rows`, and the resulting centimetres-per-pixel must match on both axes - an orthographic frame has no orthoHeight, so a non-square scale is unrenderable and is refused rather than stretched. Exactly one of cmPerPixel / (cols + rows)."),
        RPC_PARAM_OPT("rows", "number", "Explicit row count. See `cols`."),
        RPC_PARAM_DEF("tilePixels", "number",
            "Per-tile pixel size; tiles are square in pixels. Fixed for the whole burst by construction - the render target is allocated once - which is what keeps this verb clear of the capture-resize hazard that costs unsaved level state on the viewport path.",
            "768"),
        RPC_PARAM_OPT("depthCm", "number",
            "Override the georeference's measurement plane on the depth axis. Defaults to the midpoint of worldMin/worldMax on that axis, exactly as image.tile defines it."),
        RPC_PARAM_OPT("cameraDepthCm", "number",
            "Where the orthographic camera plane sits on the depth axis. Omit to derive it from the world's own measured bounds plus `cameraClearanceCm`. The scene-capture orthographic projection hardcodes its near plane to 0, so anything BEHIND the camera plane is clipped outright - the response reports the depth used, the measured scene bounds, and how much content (if any) the plane cuts off."),
        RPC_PARAM_DEF("cameraClearanceCm", "number",
            "How far beyond the measured scene bounds a derived camera is placed. Ignored when cameraDepthCm is supplied.",
            "10000"),
        RPC_PARAM_OPT("outputDir", "filepath",
            "Directory to write the tiles and manifest into. Defaults to <ProjectSaved>/PinWright/image/ortho/<namePrefix>."),
        RPC_PARAM_OPT("namePrefix", "string",
            "Filename stem for the tiles and manifest. Defaults to 'ortho'. Tiles are named <prefix>_r<row>c<col>.png, zero-padded to the grid's digit count - the same naming image.tile uses, so a capture set and a reference set pair by filename."),
        RPC_PARAM_DEF("overwrite", "boolean",
            "Replace tiles/manifest that already exist at the target paths. When false (the default) the call refuses before rendering anything, so a half-overwritten tile set is unreachable.",
            "false"),
        RPC_PARAM_DEF("allowBlank", "boolean",
            "Accept a burst in which EVERY tile read back all-zero. Blank tiles are always written and always counted; this only decides whether an entirely blank burst is a success. Default false.",
            "false"),
        // The shared description, plus the paragraph that is only true here. Concatenated at the
        // call site rather than forked, so the vocabulary stays single-definition: the macro is
        // owned by PreviewViewportCaptureUtils.h and this verb does not edit it. The suffix exists
        // because two sentences of the shared text describe a VIEWPORT this verb does not have,
        // and shipping them unqualified would promise a restore that never happens.
        RPC_PARAM_OPT("viewMode", "string", PINWRIGHT_VIEW_MODE_PARAM_DESC
            " ON THIS VERB the two viewport sentences above do not apply: render.capture_ortho_tiles"
            " renders through a transient USceneCaptureComponent2D, so there is no perspective or"
            " orthographic view-mode slot to write and NOTHING TO RESTORE - the component is created"
            " per call and discarded, and the response reports no `restored` rather than faking one."
            " The mode is written into the component's own FEngineShowFlags (which become the view"
            " family's flags verbatim) and reported, measured, under `showFlags.viewMode`:"
            " `applied` carries the key only when every show flag the mode distinguishes itself by"
            " read back off the component, `showFlagMismatches` names the ones that did not, and"
            " `derived` is the engine's own FindViewMode read of the resulting flags. Two families"
            " are REFUSED by name rather than rendered as a plausible near-Lit picture: the"
            " sub-visualisation modes (VisualizeBuffer, VisualizeSubstrate, VisualizeNanite,"
            " VisualizeLumen, RayTracingDebug, ...), whose picture needs an FSceneViewFamily::ViewMode"
            " and a ViewModeParam the scene-capture renderer never writes, and the editor debug"
            " families (LightmapDensity, LitLightmapDensity, StationaryLightOverlap,"
            " collisionSimple, collisionComplex), which draw through the editor viewport's"
            " per-mesh material substitution. Take either family through editor.set_view_mode plus"
            " render.capture_open_level instead. Note that any mode clearing Lighting or"
            " PostProcessing (unlit, wireframe, the complexity modes) also stops an `exposure` pin"
            " from governing the pixels - the response says so and names the mode as the cause.")
    ))
{
    using namespace PinWrightImage;
    using namespace PinWrightOrthoTiles;
    using namespace PinWrightTileGrid;

    const double BurstStart = FPlatformTime::Seconds();

    // ---- axes (required; there is no default that is right for both top-down poses) ----
    const TSharedPtr<FJsonObject>& Payload = Ctx.GetRawPayload();
    if (!Payload.IsValid() || !Payload->HasField(TEXT("axes")))
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
            TEXT("'axes' is required: pass a preset name (top_down_x_up_y_right | top_down_x_right_y_down | front_y_right_z_up | side_x_right_z_up) or {screenXAxis, screenXPositive, screenYAxis, screenYPositive}. There is no default - the two top-down presets are both correct and produce transposed images."));
        return true;
    }
    FScreenAxisMapping Mapping;
    FString ErrMsg;
    if (!ParseAxes(Payload->TryGetField(TEXT("axes")), Mapping, ErrMsg))
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_GEOREFERENCE, ErrMsg);
        return true;
    }

    // ---- the camera pose the axes imply, VERIFIED by classifying it back ----
    FRotator CameraRotation = FRotator::ZeroRotator;
    if (!TryMakeCameraRotationForAxisMapping(Mapping, CameraRotation, ErrMsg))
    {
        Ctx.SendError(ErrorCodes::ERR_AXIS_MAPPING_UNRENDERABLE, ErrMsg);
        return true;
    }
    const FVector CameraForward = CameraForwardForAxisMapping(Mapping);
    const EWorldAxis DepthAxis = Mapping.DepthAxis();
    const double ForwardOnDepthAxis = GetAxisValue(CameraForward, DepthAxis);
    if (FMath::Abs(ForwardOnDepthAxis) < 0.5)
    {
        // Unreachable through a valid mapping (forward is a cardinal unit vector on the depth
        // axis by construction), and refused rather than assumed because the whole camera
        // placement below divides the world into "in front" and "clipped" on this sign.
        Ctx.SendError(ErrorCodes::ERR_AXIS_MAPPING_UNRENDERABLE,
            FString::Printf(TEXT("camera forward %s does not run along the depth axis %s"),
                *CameraForward.ToString(), AxisName(DepthAxis)));
        return true;
    }

    // ---- the requested world box ----
    FVector RequestedMin = FVector::ZeroVector;
    FVector RequestedMax = FVector::ZeroVector;
    if (!ParseWorldPointField(Payload, TEXT("worldMin"), RequestedMin, ErrMsg) ||
        !ParseWorldPointField(Payload, TEXT("worldMax"), RequestedMax, ErrMsg))
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, ErrMsg);
        return true;
    }
    const FWorldExtent2D RequestedExtent = MakeWorldExtentFromBounds(RequestedMin, RequestedMax, Mapping);
    if (!RequestedExtent.IsValid())
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_GEOREFERENCE,
            FString::Printf(
                TEXT("worldMin/worldMax span nothing in the capture plane (%s span %.3f cm, %s span %.3f cm). Both in-plane axes need a strictly positive span; the depth axis (%s) may be flat."),
                AxisName(Mapping.ScreenXAxis), RequestedExtent.SpanAcross(),
                AxisName(Mapping.ScreenYAxis), RequestedExtent.SpanDown(),
                AxisName(DepthAxis)));
        return true;
    }

    // ---- tile pixel size, resolved ONCE for the whole burst ----
    const int32 TilePixels = Ctx.GetInt(TEXT("tilePixels"), PwOrthoDefaultTilePixels);
    if (TilePixels < MinTilePixels || TilePixels > MaxTilePixels)
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
            FString::Printf(TEXT("tilePixels %d is outside [%d, %d]"),
                TilePixels, MinTilePixels, MaxTilePixels));
        return true;
    }

    // ---- the grid: exactly one of cmPerPixel / (cols + rows) ----
    const bool bHasCmPerPixel = Payload->HasField(TEXT("cmPerPixel"));
    const bool bHasCols = Payload->HasField(TEXT("cols"));
    const bool bHasRows = Payload->HasField(TEXT("rows"));
    if (bHasCmPerPixel == (bHasCols || bHasRows))
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
            TEXT("supply EXACTLY one of cmPerPixel or (cols + rows). Neither leaves the subdivision undefined; both would let a stale cmPerPixel silently win over a corrected cols/rows."));
        return true;
    }

    FTileGrid Grid;
    bool bExtentExpanded = false;
    double PlannedCmPerPixel = 0.0;
    if (bHasCmPerPixel)
    {
        const double CmPerPixel = Ctx.GetNumber(TEXT("cmPerPixel"), 0.0);
        if (!(CmPerPixel > 0.0))
        {
            Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
                FString::Printf(TEXT("cmPerPixel must be > 0 (got %.6f)"), CmPerPixel));
            return true;
        }
        FTileGridPlan Plan;
        // MaxTiles is 0 (uncapped) here on purpose: the budget refusal below carries a typed code
        // and the predicted cost, which a string returned from the planner cannot.
        if (!PlanTileGrid(RequestedExtent, Mapping, TilePixels, TilePixels, CmPerPixel,
            /*MaxTiles=*/0, Plan, ErrMsg))
        {
            Ctx.SendError(ErrorCodes::ERR_INVALID_GEOREFERENCE, ErrMsg);
            return true;
        }
        Grid = Plan.Grid;
        bExtentExpanded = Plan.bExtentExpanded;
        PlannedCmPerPixel = Plan.WorldUnitsPerPixel;
    }
    else
    {
        if (!bHasCols || !bHasRows)
        {
            Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
                TEXT("cols and rows must be supplied together"));
            return true;
        }
        const int32 Cols = Ctx.GetInt(TEXT("cols"), 0);
        const int32 Rows = Ctx.GetInt(TEXT("rows"), 0);
        if (Cols < 1 || Rows < 1 || Cols > MaxTilesPerAxis || Rows > MaxTilesPerAxis)
        {
            Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
                FString::Printf(TEXT("cols and rows must each be in [1, %d] (got %d x %d)"),
                    MaxTilesPerAxis, Cols, Rows));
            return true;
        }
        if (!MakeTileGrid(RequestedExtent, Mapping, Cols, Rows, TilePixels, TilePixels, Grid, ErrMsg))
        {
            Ctx.SendError(ErrorCodes::ERR_INVALID_GEOREFERENCE, ErrMsg);
            return true;
        }
        if (!Grid.HasSquarePixels())
        {
            Ctx.SendError(ErrorCodes::ERR_INVALID_GEOREFERENCE,
                FString::Printf(
                    TEXT("a %d x %d subdivision of this extent gives %.6f cm/px across (%s) and %.6f cm/px down (%s). An orthographic frame has only an orthoWidth - its vertical span is forced to orthoWidth * height / width - so a non-square scale cannot be rendered. Change cols/rows, change the extent, or pass cmPerPixel and let the planner choose."),
                    Cols, Rows, Grid.WorldUnitsPerPixelAcross(), AxisName(Mapping.ScreenXAxis),
                    Grid.WorldUnitsPerPixelDown(), AxisName(Mapping.ScreenYAxis)));
            return true;
        }
        PlannedCmPerPixel = Grid.WorldUnitsPerPixelAcross();
    }

    // ---- burst ceilings, checked before any work ----
    const int64 TilesRequested = Grid.TileCount();
    const int64 TotalPixels = TilesRequested * static_cast<int64>(TilePixels) * static_cast<int64>(TilePixels);
    const double PredictedSeconds =
        (static_cast<double>(TotalPixels) / 1048576.0) * MeasuredSecondsPerMegapixel;
    if (TilesRequested > static_cast<int64>(PinWrightOrthoTiles::MaxTilesPerCall) ||
        TotalPixels > MaxTotalPixelsPerCall)
    {
        TSharedPtr<FJsonObject> BudgetDetail = MakeShared<FJsonObject>();
        BudgetDetail->SetNumberField(TEXT("tilesRequested"), static_cast<double>(TilesRequested));
        BudgetDetail->SetNumberField(TEXT("cols"), Grid.Cols);
        BudgetDetail->SetNumberField(TEXT("rows"), Grid.Rows);
        BudgetDetail->SetNumberField(TEXT("tilePixels"), TilePixels);
        BudgetDetail->SetNumberField(TEXT("totalPixels"), static_cast<double>(TotalPixels));
        BudgetDetail->SetNumberField(TEXT("maxTilesPerCall"), PinWrightOrthoTiles::MaxTilesPerCall);
        BudgetDetail->SetNumberField(TEXT("maxTotalPixelsPerCall"), static_cast<double>(MaxTotalPixelsPerCall));
        BudgetDetail->SetNumberField(TEXT("predictedSeconds"), PredictedSeconds);
        BudgetDetail->SetNumberField(TEXT("measuredSecondsPerMegapixel"), MeasuredSecondsPerMegapixel);
        Ctx.SendError(ErrorCodes::ERR_TILE_BUDGET_EXCEEDED,
            FString::Printf(
                TEXT("%lld tiles (%d x %d) at %d px is %lld pixels, predicted ~%.0f s at the measured %.3f s/MP; the ceilings are %d tiles and %lld pixels. Nothing was rendered. This verb is synchronous - Ctx.StartJob invokes its bind delegate on the caller's own stack, so a ticket would buy bookkeeping and not cancellation - and a burst past these ceilings outlives the transport's response timeout, leaving the render running with nobody watching. Raise cmPerPixel, lower tilePixels, or split the extent across several calls."),
                TilesRequested, Grid.Cols, Grid.Rows, TilePixels, TotalPixels, PredictedSeconds,
                MeasuredSecondsPerMegapixel, PinWrightOrthoTiles::MaxTilesPerCall, MaxTotalPixelsPerCall),
            BudgetDetail);
        return true;
    }

    // ---- exposure: required, and one pin for the whole burst ----
    FString ErrCode;
    PinWrightRenderCapture::FExposurePin ExposurePin;
    if (!PinWrightRenderCapture::ParseExposurePin(Payload, ExposurePin, ErrCode, ErrMsg))
    {
        Ctx.SendError(ErrCode, ErrMsg);
        return true;
    }
    if (ExposurePin.Mode == PinWrightRenderCapture::EExposureRequestMode::Unset)
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
            TEXT("'exposure' is required on this verb. A scene capture keeps no persistent view state, so under auto-exposure each tile exposes to its own content and the mosaic steps at every seam. Pass a number (EV100) or {mode:\"fixed\", ev100:N} to pin one exposure for the whole burst, or {mode:\"auto\"} to accept per-tile exposure deliberately."));
        return true;
    }

    // ---- viewMode: two gates, and both run before anything is rendered or written ----
    //
    // Both are pure functions of the payload, so they sit HERE, above the world lookup, the bounds
    // survey and the output-directory checks: a malformed or unreachable mode is refused without
    // touching the level or the filesystem, and the error a caller sees does not depend on whether
    // a world happened to be loaded.
    //
    // No client is passed to ParseViewModePin - there is no viewport on this path to pass. That
    // makes the ten sub-visualisation modes refuse here with VIEW_MODE_NEEDS_COMPANION, which is
    // the right answer for this verb permanently rather than only until a target is selected: even
    // WITH one selected the scene-capture renderer cannot draw them (see
    // IsViewModeReachableOnSceneCapture, which is the second gate and catches them regardless).
    PinWrightRenderCapture::FViewModePin ViewModePin;
    if (!PinWrightRenderCapture::ParseViewModePin(Payload, ViewModePin, ErrCode, ErrMsg))
    {
        Ctx.SendError(ErrCode, ErrMsg);
        return true;
    }
    if (ViewModePin.WantsOverride() &&
        !IsViewModeReachableOnSceneCapture(ViewModePin.ViewMode, ErrCode, ErrMsg))
    {
        Ctx.SendError(ErrCode, ErrMsg);
        return true;
    }

    // ---- the georeference's measurement plane ----
    // Same definition as image.tile: the midpoint of the AABB's depth span unless overridden.
    // This is NOT where the camera goes - see cameraDepthCm below.
    const double RequestedDepthMin = GetAxisValue(RequestedMin, DepthAxis);
    const double RequestedDepthMax = GetAxisValue(RequestedMax, DepthAxis);
    const bool bDepthExplicit = Payload->HasField(TEXT("depthCm"));
    const double GeoDepthCm = bDepthExplicit
        ? Ctx.GetNumber(TEXT("depthCm"), 0.0)
        : 0.5 * (RequestedDepthMin + RequestedDepthMax);

    // ---- the world ----
    if (!GEditor)
    {
        Ctx.SendError(ErrorCodes::ERR_EDITOR_NOT_AVAILABLE, TEXT("Editor not available"));
        return true;
    }
    UWorld* EditorWorld = GEditor->GetEditorWorldContext().World();
    if (!EditorWorld)
    {
        Ctx.SendError(ErrorCodes::ERR_NO_EDITOR_WORLD, TEXT("No active editor world"));
        return true;
    }

    // ---- camera depth: measured, or supplied and then checked against the measurement ----
    const FDepthBoundsSurvey Survey = SurveyWorldDepthBounds(EditorWorld, DepthAxis);
    const bool bCameraDepthExplicit = Payload->HasField(TEXT("cameraDepthCm"));
    const double CameraClearanceCm = Ctx.GetNumber(TEXT("cameraClearanceCm"), DefaultCameraClearanceCm);
    double CameraDepthCm = 0.0;
    if (bCameraDepthExplicit)
    {
        CameraDepthCm = Ctx.GetNumber(TEXT("cameraDepthCm"), 0.0);
    }
    else if (!Survey.bMeasured)
    {
        TSharedPtr<FJsonObject> Detail = MakeShared<FJsonObject>();
        Detail->SetNumberField(TEXT("primitivesSurveyed"), Survey.NumPrimitives);
        Detail->SetStringField(TEXT("depthAxis"), AxisName(DepthAxis));
        Ctx.SendError(ErrorCodes::ERR_SCENE_BOUNDS_NOT_MEASURED,
            FString::Printf(
                TEXT("cameraDepthCm was omitted and the world carries no visible registered primitive with bounds (%d surveyed), so there is nothing to place the camera behind. The orthographic projection's near plane is hardcoded to 0, so a guessed camera plane clips whatever sits behind it. Pass cameraDepthCm explicitly."),
                Survey.NumPrimitives),
            Detail);
        return true;
    }
    else
    {
        // Behind the scene along the view direction: looking down -Z means sitting above the
        // highest bound, looking along +X means sitting below the lowest.
        CameraDepthCm = (ForwardOnDepthAxis < 0.0)
            ? Survey.DepthMax + CameraClearanceCm
            : Survey.DepthMin - CameraClearanceCm;
    }

    // How much surveyed content the near-plane-0 clip cuts off, measured either way. Positive
    // means real geometry sits behind the camera plane and is missing from every tile.
    double ContentBehindCameraCm = 0.0;
    if (Survey.bMeasured)
    {
        ContentBehindCameraCm = (ForwardOnDepthAxis < 0.0)
            ? FMath::Max(0.0, Survey.DepthMax - CameraDepthCm)
            : FMath::Max(0.0, CameraDepthCm - Survey.DepthMin);
    }

    // ---- output targets, all resolved and checked BEFORE anything is rendered ----
    const FString Prefix = SanitizeBaseName(Ctx.GetString(TEXT("namePrefix")), TEXT("ortho"));
    const FString OutputDir = ResolveOutputDir(Ctx.GetString(TEXT("outputDir")),
        FString(TEXT("ortho")) / Prefix);
    const bool bOverwrite = Ctx.GetBool(TEXT("overwrite"), false);
    const bool bAllowBlank = Ctx.GetBool(TEXT("allowBlank"), false);

    const int32 ColDigits = FString::FromInt(FMath::Max(0, Grid.Cols - 1)).Len();
    const int32 RowDigits = FString::FromInt(FMath::Max(0, Grid.Rows - 1)).Len();
    const FString ManifestPath = OutputDir / (Prefix + TEXT("_manifest.json"));

    TArray<FString> TilePaths;
    TilePaths.Reserve(static_cast<int32>(TilesRequested));
    TArray<FString> Existing;
    for (int32 Row = 0; Row < Grid.Rows; ++Row)
    {
        for (int32 Col = 0; Col < Grid.Cols; ++Col)
        {
            const FString Path = OutputDir / PwOrthoTileFileName(Prefix, Col, Row, ColDigits, RowDigits);
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
        TSharedPtr<FJsonObject> Detail = MakeShared<FJsonObject>();
        TArray<TSharedPtr<FJsonValue>> Blocking;
        for (const FString& Path : Existing)
        {
            Blocking.Add(MakeShared<FJsonValueString>(Path));
        }
        Detail->SetArrayField(TEXT("existingFiles"), Blocking);
        Detail->SetStringField(TEXT("outputDir"), OutputDir);
        Ctx.SendError(ErrorCodes::ERR_ALREADY_EXISTS,
            FString::Printf(TEXT("%d output file(s) already exist under %s and overwrite is false. Nothing was rendered or written - pass overwrite:true, or choose another outputDir/namePrefix."),
                Existing.Num(), *OutputDir),
            Detail);
        return true;
    }

    // ---- the georeference this burst publishes ----
    // Built on the COVERED extent (which PlanTileGrid may have expanded), with the depth axis at
    // the measurement plane, so the corners a reader gets back describe the ground actually
    // rendered rather than the ground asked for.
    FGeoreference Geo;
    Geo.Grid = Grid;
    Geo.DepthCm = GeoDepthCm;
    Geo.bDepthExplicit = bDepthExplicit;
    SetAxisValue(Geo.WorldMin, Mapping.ScreenXAxis, Grid.World.Min.X);
    SetAxisValue(Geo.WorldMin, Mapping.ScreenYAxis, Grid.World.Min.Y);
    SetAxisValue(Geo.WorldMin, DepthAxis, GeoDepthCm);
    SetAxisValue(Geo.WorldMax, Mapping.ScreenXAxis, Grid.World.Max.X);
    SetAxisValue(Geo.WorldMax, Mapping.ScreenYAxis, Grid.World.Max.Y);
    SetAxisValue(Geo.WorldMax, DepthAxis, GeoDepthCm);

    // ---- the level's dirty state, sampled before the capture ----
    const int32 DirtyLevelPackagesBefore = PwOrthoCountDirtyLevelPackages(EditorWorld);

    // ---- render ----
    FOrthoTileCapture Capture(EditorWorld, TilePixels, TilePixels, ExposurePin, ViewModePin);
    if (!Capture.IsValid())
    {
        Ctx.SendError(Capture.GetInitErrorCode().IsEmpty()
                ? FString(ErrorCodes::ERR_SCENE_CAPTURE_FAILED) : Capture.GetInitErrorCode(),
            Capture.GetInitErrorMessage().IsEmpty()
                ? FString(TEXT("Could not initialise the orthographic capture")) : Capture.GetInitErrorMessage());
        return true;
    }

    IFileManager::Get().MakeDirectory(*OutputDir, true);

    TArray<TSharedPtr<FJsonValue>> TileEntries;
    TileEntries.Reserve(static_cast<int32>(TilesRequested));
    TSet<FString> WrittenNames;
    int32 TilesWritten = 0;
    int32 BlankTiles = 0;
    double BurstMeanLuminance = 0.0;
    double BurstMaxLuminance = 0.0;
    double TotalRenderMs = 0.0;
    double TotalReadbackMs = 0.0;
    double TotalEncodeMs = 0.0;
    double MinTileMs = TNumericLimits<double>::Max();
    double MaxTileMs = 0.0;

    for (int32 Row = 0; Row < Grid.Rows; ++Row)
    {
        for (int32 Col = 0; Col < Grid.Cols; ++Col)
        {
            const FTileIndex Tile(Col, Row);
            const int32 Flat = Row * Grid.Cols + Col;

            FTileCapturePlan Plan;
            if (!ComputeTileCapturePlan(Grid, Tile, CameraDepthCm, Plan, ErrMsg))
            {
                Ctx.SendError(ErrorCodes::ERR_TILE_OUT_OF_RANGE,
                    FString::Printf(TEXT("Could not plan tile (%d,%d): %s. %d of %lld tiles were already written to %s."),
                        Col, Row, *ErrMsg, TilesWritten, TilesRequested, *OutputDir));
                return true;
            }

            FBitmap TileBitmap;
            FTileTiming Timing;
            PinWrightRenderCapture::FCaptureImageStats Stats;
            FTileCameraPose Pose;
            if (!Capture.CaptureTile(Plan, CameraRotation, TileBitmap.Pixels, Timing, Stats, Pose, ErrCode, ErrMsg))
            {
                Ctx.SendError(ErrCode,
                    FString::Printf(TEXT("%s (tile %d,%d; %d of %lld tiles were already written to %s)"),
                        *ErrMsg, Col, Row, TilesWritten, TilesRequested, *OutputDir));
                return true;
            }
            TileBitmap.Width = TilePixels;
            TileBitmap.Height = TilePixels;

            // Blankness BEFORE the alpha stamp: ForceOpaqueAlpha rewrites A to 0xFF on every
            // pixel, which turns a detectably-empty all-zero readback into a plausible opaque
            // black frame and destroys the only signal there is.
            const bool bBlank = PinWrightScreenshotUtils::IsBlankReadback(TileBitmap.Pixels);
            BlankTiles += bBlank ? 1 : 0;
            BurstMeanLuminance += Stats.MeanLuminance;
            BurstMaxLuminance = FMath::Max(BurstMaxLuminance, Stats.MaxLuminance);
            PinWrightScreenshotUtils::ForceOpaqueAlpha(TileBitmap.Pixels);

            const double EncodeStart = FPlatformTime::Seconds();
            if (!SaveBitmapPng(TilePaths[Flat], TileBitmap, ErrCode, ErrMsg))
            {
                Ctx.SendError(ErrCode,
                    FString::Printf(TEXT("%s (tile %d,%d; %d of %lld tiles were already written to %s)"),
                        *ErrMsg, Col, Row, TilesWritten, TilesRequested, *OutputDir));
                return true;
            }
            const double EncodeMs = (FPlatformTime::Seconds() - EncodeStart) * 1000.0;
            ++TilesWritten;
            WrittenNames.Add(FPaths::GetCleanFilename(TilePaths[Flat]));

            TotalRenderMs += Timing.RenderMs;
            TotalReadbackMs += Timing.ReadbackMs;
            TotalEncodeMs += EncodeMs;
            const double TileMs = Timing.TotalMs + EncodeMs;
            MinTileMs = FMath::Min(MinTileMs, TileMs);
            MaxTileMs = FMath::Max(MaxTileMs, TileMs);

            const FVector2D Origin = TilePixelToPixel(Grid, Tile, FVector2D::ZeroVector);
            FVector TileMin = FVector::ZeroVector;
            FVector TileMax = FVector::ZeroVector;
            TSharedPtr<FJsonObject> TileGeo;
            if (!PwOrthoMakeTileGeoreference(Geo, Tile, TileMin, TileMax, TileGeo))
            {
                Ctx.SendError(ErrorCodes::ERR_TILE_OUT_OF_RANGE,
                    FString::Printf(TEXT("Tile (%d,%d) has no world extent in a %d x %d grid"),
                        Col, Row, Grid.Cols, Grid.Rows));
                return true;
            }

            TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
            Entry->SetNumberField(TEXT("col"), Col);
            Entry->SetNumberField(TEXT("row"), Row);
            Entry->SetStringField(TEXT("file"), FPaths::GetCleanFilename(TilePaths[Flat]));
            Entry->SetStringField(TEXT("path"), TilePaths[Flat]);
            Entry->SetNumberField(TEXT("pixelOriginX"), Origin.X);
            Entry->SetNumberField(TEXT("pixelOriginY"), Origin.Y);
            Entry->SetNumberField(TEXT("pixelWidth"), Grid.TilePixelWidth);
            Entry->SetNumberField(TEXT("pixelHeight"), Grid.TilePixelHeight);
            Entry->SetObjectField(TEXT("worldMin"), MakeVectorObject(TileMin));
            Entry->SetObjectField(TEXT("worldMax"), MakeVectorObject(TileMax));
            Entry->SetObjectField(TEXT("worldCentre"), MakeVectorObject((TileMin + TileMax) * 0.5));
            // The pose the pixels were drawn from, read off the component - not the planned one.
            Entry->SetObjectField(TEXT("cameraLocation"), MakeVectorObject(Pose.Location));
            Entry->SetObjectField(TEXT("cameraLocationPlanned"), MakeVectorObject(Plan.CameraLocation));
            Entry->SetNumberField(TEXT("orthoWidthCm"), Plan.OrthoWidth);
            Entry->SetBoolField(TEXT("blank"), bBlank);
            // Reported, never judged: the verb states what the pixels measure and leaves "is this
            // picture usable" to the reader. An all-zero `blank` and a mean luminance of 0.001
            // are different facts and only the second one catches a frame drawn over nothing.
            Entry->SetNumberField(TEXT("meanLuminance"), Stats.MeanLuminance);
            Entry->SetNumberField(TEXT("maxLuminance"), Stats.MaxLuminance);
            Entry->SetNumberField(TEXT("minLuminance"), Stats.MinLuminance);
            Entry->SetNumberField(TEXT("luminanceVariance"), Stats.LuminanceVariance);
            Entry->SetNumberField(TEXT("renderMs"), Timing.RenderMs);
            Entry->SetNumberField(TEXT("readbackMs"), Timing.ReadbackMs);
            Entry->SetNumberField(TEXT("encodeMs"), EncodeMs);
            Entry->SetNumberField(TEXT("totalMs"), TileMs);
            Entry->SetObjectField(TEXT("georeference"), TileGeo);
            TileEntries.Add(MakeShared<FJsonValueObject>(Entry));
        }
    }

    const int32 DirtyLevelPackagesAfter = PwOrthoCountDirtyLevelPackages(EditorWorld);
    const double BurstSeconds = FPlatformTime::Seconds() - BurstStart;
    const FExposurePlan& Exposure = Capture.GetExposurePlan();

    // ---- shared blocks, built once and put in both the manifest and the response ----
    TSharedPtr<FJsonObject> CameraObj = MakeShared<FJsonObject>();
    CameraObj->SetObjectField(TEXT("rotation"), PwOrthoMakeRotatorObject(CameraRotation));
    CameraObj->SetObjectField(TEXT("forward"), MakeVectorObject(CameraForward));
    CameraObj->SetStringField(TEXT("depthAxis"), AxisName(DepthAxis));
    CameraObj->SetNumberField(TEXT("depthCm"), CameraDepthCm);
    CameraObj->SetStringField(TEXT("depthSource"), bCameraDepthExplicit ? TEXT("caller") : TEXT("sceneBounds"));
    CameraObj->SetNumberField(TEXT("clearanceCm"), bCameraDepthExplicit ? 0.0 : CameraClearanceCm);
    // The near plane is not a setting this verb chose - BuildOrthoMatrix hardcodes it - so it is
    // reported as the constant it is, beside the depth that had to clear it.
    CameraObj->SetNumberField(TEXT("nearPlaneCm"), 0.0);
    CameraObj->SetNumberField(TEXT("contentBehindCameraCm"), ContentBehindCameraCm);
    CameraObj->SetBoolField(TEXT("updateOrthoPlanes"), Capture.MeasureUpdateOrthoPlanes());
    TSharedPtr<FJsonObject> BoundsObj = MakeShared<FJsonObject>();
    BoundsObj->SetBoolField(TEXT("measured"), Survey.bMeasured);
    BoundsObj->SetNumberField(TEXT("primitives"), Survey.NumPrimitives);
    // Primitives whose bounds reach world scale (sky, fog, unbound volumes) contribute no extent
    // and are excluded. Published because the exclusion changes where the camera goes.
    BoundsObj->SetNumberField(TEXT("unboundedPrimitivesExcluded"), Survey.NumUnbounded);
    if (Survey.bMeasured)
    {
        BoundsObj->SetObjectField(TEXT("worldMin"), MakeVectorObject(Survey.BoundsMin));
        BoundsObj->SetObjectField(TEXT("worldMax"), MakeVectorObject(Survey.BoundsMax));
        BoundsObj->SetNumberField(TEXT("depthMin"), Survey.DepthMin);
        BoundsObj->SetNumberField(TEXT("depthMax"), Survey.DepthMax);
    }
    CameraObj->SetObjectField(TEXT("sceneBounds"), BoundsObj);

    TSharedPtr<FJsonObject> ExposureObj = MakeShared<FJsonObject>();
    ExposureObj->SetStringField(TEXT("mode"), PinWrightRenderCapture::ExposureModeKey(Exposure.Mode));
    ExposureObj->SetBoolField(TEXT("pinRequested"),
        Exposure.Mode == PinWrightRenderCapture::EExposureRequestMode::Fixed);
    ExposureObj->SetBoolField(TEXT("pinned"), Exposure.bPinned);
    ExposureObj->SetNumberField(TEXT("ev100Requested"), Exposure.Ev100Requested);
    ExposureObj->SetBoolField(TEXT("ev100Measured"), Exposure.bEv100Measured);
    if (Exposure.bEv100Measured)
    {
        ExposureObj->SetNumberField(TEXT("ev100Applied"), Exposure.Ev100Applied);
    }
    ExposureObj->SetStringField(TEXT("method"),
        Exposure.Mode == PinWrightRenderCapture::EExposureRequestMode::Fixed
            ? TEXT("manualPhysicalCamera") : TEXT("sceneDefault"));
    if (Exposure.Mode == PinWrightRenderCapture::EExposureRequestMode::Fixed)
    {
        ExposureObj->SetNumberField(TEXT("fstop"), Exposure.Fstop);
        ExposureObj->SetNumberField(TEXT("shutterSpeed"), Exposure.ShutterSpeed);
        ExposureObj->SetNumberField(TEXT("iso"), Exposure.Iso);
    }
    if (!Exposure.BlockedReason.IsEmpty())
    {
        ExposureObj->SetStringField(TEXT("pinWarning"), Exposure.BlockedReason);
    }

    TSharedPtr<FJsonObject> TimingObj = MakeShared<FJsonObject>();
    TimingObj->SetNumberField(TEXT("totalSeconds"), BurstSeconds);
    TimingObj->SetNumberField(TEXT("renderMs"), TotalRenderMs);
    TimingObj->SetNumberField(TEXT("readbackMs"), TotalReadbackMs);
    TimingObj->SetNumberField(TEXT("encodeMs"), TotalEncodeMs);
    TimingObj->SetNumberField(TEXT("minTileMs"), TilesWritten > 0 ? MinTileMs : 0.0);
    TimingObj->SetNumberField(TEXT("maxTileMs"), MaxTileMs);
    TimingObj->SetNumberField(TEXT("meanTileMs"),
        TilesWritten > 0 ? (TotalRenderMs + TotalReadbackMs + TotalEncodeMs) / TilesWritten : 0.0);
    TimingObj->SetNumberField(TEXT("predictedSeconds"), PredictedSeconds);
    TimingObj->SetNumberField(TEXT("totalPixels"), static_cast<double>(TotalPixels));
    TimingObj->SetNumberField(TEXT("renders"), Capture.GetRenderCount());

    TSharedPtr<FJsonObject> ImageObj = MakeShared<FJsonObject>();
    ImageObj->SetNumberField(TEXT("meanLuminance"),
        TilesWritten > 0 ? BurstMeanLuminance / TilesWritten : 0.0);
    ImageObj->SetNumberField(TEXT("maxLuminance"), BurstMaxLuminance);
    ImageObj->SetNumberField(TEXT("blankTiles"), BlankTiles);
    ImageObj->SetNumberField(TEXT("tilesMeasured"), TilesWritten);

    TSharedPtr<FJsonObject> LevelObj = MakeShared<FJsonObject>();
    LevelObj->SetNumberField(TEXT("dirtyPackagesBefore"), DirtyLevelPackagesBefore);
    LevelObj->SetNumberField(TEXT("dirtyPackagesAfter"), DirtyLevelPackagesAfter);
    // Measured, not claimed. The capture component is ownerless, transient and outered to the
    // transient package, so no level package should change state - this is the observation that
    // says so, and it is a comparison of two counts rather than a literal.
    LevelObj->SetBoolField(TEXT("dirtiedByCapture"),
        DirtyLevelPackagesBefore >= 0 && DirtyLevelPackagesAfter > DirtyLevelPackagesBefore);
    LevelObj->SetStringField(TEXT("levelPath"),
        EditorWorld->GetOutermost() ? EditorWorld->GetOutermost()->GetName() : FString());

    // ---- landscape grass ----
    //
    // Unconditional, because its absence is exactly what made this verb unusable for vegetation
    // review: tiles came back valid, non-blank and settled over ground the perspective capture
    // fills with grass, and no field in the response could contradict them
    // (B-ortho-capture-renders-no-landscape-grass). `instances` with `reach.instancesInReach`
    // separates "this ground has no grass" from "the grass exists and none of it can be in this
    // frame"; `settled` separates both from "the build had not finished". The burst-level pair
    // below is added only when there is terrain, so a level with none is not given two numbers
    // about nothing.
    TSharedPtr<FJsonObject> GrassObj =
        PinWrightCaptureGrass::MakeGrassBuildInfoObject(Capture.GetGrassBuild());
    if (Capture.GetGrassBuild().bMeasured && Capture.GetGrassBuild().LandscapeProxies > 0)
    {
        // Summed over every tile, not the last tile's slice: the cost a caller is being asked to
        // pay for the burst. `allTilesSettled` is ANDed over the burst for the same reason -- the
        // last tile settling says nothing about the first one's pixels.
        GrassObj->SetNumberField(TEXT("buildMsTotal"), Capture.GetGrassBuildTotalMs());
        GrassObj->SetBoolField(TEXT("allTilesSettled"), Capture.AllTilesGrassSettled());
    }

    // ---- manifest ----
    // Shaped so PinWrightImage::LoadGeoreferenceObjectFromFile reads it verbatim: a root
    // `georeference` plus a `tiles` array whose entries carry `col`, `row` and their own
    // `georeference`. That is what makes `image.tile {georeferenceFrom: <this manifest>}` cut a
    // reference image onto exactly this ground, and `image.compare` able to pair tile for tile.
    TSharedPtr<FJsonObject> Manifest = MakeShared<FJsonObject>();
    Manifest->SetStringField(TEXT("schema"), TEXT("pinwright.render.capture_ortho_tiles/1"));
    Manifest->SetStringField(TEXT("generatedUtc"), FDateTime::UtcNow().ToIso8601());
    // Recorded because two captures from different renderers are NOT comparable: viewport vs.
    // scene capture measures 5.76% mean absolute error against a 1.30% viewport self-noise floor,
    // and an ideal fitted tone LUT only closes it to 3.28%. A comparison tool that finds two
    // different values here should refuse the pair rather than report the difference as content.
    Manifest->SetStringField(TEXT("renderer"), PinWrightCaptureRenderer::SceneCapture2D);
    // `projectionMode`, NOT `projection`. Measured 2026-08-21: six other response writers publish
    // the perspective/orthographic concept under this key (AnnotatedCaptureHandler.cpp,
    // CameraShotPlanUtils.h, RenderHandler.cpp x2, ZFightingHandler.cpp, RaycastScreenHandler.cpp),
    // and 39 source lines across 11 files plus every wiki page spell it that way. This manifest
    // was the sole outlier, so a caller comparing an ortho burst against any other capture had to
    // know two spellings for one field. `projection` still exists, on editor.set_view_mode, and
    // means something else entirely - WHICH view-mode slot to write (docs/wiki-src/editor.md).
    // Two fields cannot share one name and stay legible.
    Manifest->SetStringField(TEXT("projectionMode"), TEXT("orthographic"));
    Manifest->SetStringField(TEXT("levelPath"),
        EditorWorld->GetOutermost() ? EditorWorld->GetOutermost()->GetName() : FString());
    Manifest->SetStringField(TEXT("namePrefix"), Prefix);
    Manifest->SetObjectField(TEXT("georeference"), SerializeGeoreference(Geo));
    Manifest->SetObjectField(TEXT("grid"), DescribeGrid(Geo));
    Manifest->SetObjectField(TEXT("camera"), CameraObj);
    Manifest->SetObjectField(TEXT("exposure"), ExposureObj);
    Manifest->SetObjectField(TEXT("showFlags"), Capture.DescribeShowFlags());
    Manifest->SetObjectField(TEXT("grass"), GrassObj);
    Manifest->SetObjectField(TEXT("imageStats"), ImageObj);
    Manifest->SetBoolField(TEXT("extentExpanded"), bExtentExpanded);
    Manifest->SetObjectField(TEXT("requestedWorldMin"), MakeVectorObject(RequestedMin));
    Manifest->SetObjectField(TEXT("requestedWorldMax"), MakeVectorObject(RequestedMax));
    Manifest->SetArrayField(TEXT("tiles"), TileEntries);

    FString ManifestText;
    const TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer =
        TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&ManifestText);
    if (!FJsonSerializer::Serialize(Manifest.ToSharedRef(), Writer) ||
        !FFileHelper::SaveStringToFile(ManifestText, *ManifestPath))
    {
        Ctx.SendError(ErrorCodes::ERR_WRITE_FAILED,
            FString::Printf(TEXT("Rendered and wrote %d tiles to %s but could not write the manifest %s. The tiles on disk have no recorded georeference until this succeeds."),
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

    TArray<TSharedPtr<FJsonValue>> Warnings;
    if (bExtentExpanded)
    {
        Warnings.Add(MakeShared<FJsonValueString>(FString::Printf(
            TEXT("The captured extent is LARGER than the one requested: %.1f x %.1f cm covered vs %.1f x %.1f cm asked for, expanded symmetrically about the requested centre so the area is exactly %d x %d whole tiles at %.4f cm/px. Whole tiles at the requested scale do not divide the request, and cropping would silently drop world you asked to see. The published georeference describes the COVERED extent."),
            Grid.World.SpanAcross(), Grid.World.SpanDown(),
            RequestedExtent.SpanAcross(), RequestedExtent.SpanDown(),
            Grid.Cols, Grid.Rows, PlannedCmPerPixel)));
    }
    if (Exposure.Mode == PinWrightRenderCapture::EExposureRequestMode::Auto)
    {
        Warnings.Add(MakeShared<FJsonValueString>(
            TEXT("exposure mode is \"auto\": this capture keeps no persistent view state, so each tile's exposure was derived from that tile's own content. Neighbouring tiles will step at the seam and the mosaic is not a single image. Re-capture with a fixed ev100 before comparing anything.")));
    }
    if (Exposure.Mode == PinWrightRenderCapture::EExposureRequestMode::Fixed && !Exposure.bPinned)
    {
        // Two spellings of one fact, because the two readings need different actions. Without a
        // requested view mode this is a fault to chase; with one, the caller asked for the mode
        // that cleared the flag and the warning must not send them hunting. Same warning slot,
        // same `pinned: false`, different opening clause - and the mode is NAMED, so a reader
        // grepping the response for "unlit" finds the cause.
        const FViewModePlan& ViewModeApplied = Capture.GetViewModePlan();
        Warnings.Add(MakeShared<FJsonValueString>(ViewModeApplied.bRequested
            ? FString::Printf(
                TEXT("The exposure pin does not govern these pixels, and viewMode '%s' is why: %s"),
                *ViewModeApplied.RequestedKey, *Exposure.BlockedReason)
            : FString::Printf(
                TEXT("An exposure pin was requested but did not govern these pixels: %s. The tiles used whatever exposure the scene resolved and are not comparable with a pinned set."),
                *Exposure.BlockedReason)));
    }
    // A requested mode whose distinguishing show flags did NOT all read back off the component is
    // the one outcome that looks like success and is not: the tiles were drawn, the response says
    // a mode was requested, and only this list says the renderer never saw it.
    {
        const FViewModePlan& ViewModeApplied = Capture.GetViewModePlan();
        if (ViewModeApplied.bRequested && !ViewModeApplied.bApplied)
        {
            Warnings.Add(MakeShared<FJsonValueString>(FString::Printf(
                TEXT("viewMode '%s' was requested, but %d of the %d show flags that distinguish it from Lit did not read back off the capture component (%s). These tiles are NOT in that mode - they are whatever the remaining flags describe, which the response reports as `showFlags.viewMode.derived`: %s."),
                *ViewModeApplied.RequestedKey, ViewModeApplied.ShowFlagMismatches.Num(),
                ViewModeApplied.DistinguishingShowFlags.Num(),
                *FString::Join(ViewModeApplied.ShowFlagMismatches, TEXT(", ")),
                *ViewModeApplied.DerivedKey)));
        }
    }
    if (ContentBehindCameraCm > 0.0)
    {
        Warnings.Add(MakeShared<FJsonValueString>(FString::Printf(
            TEXT("%.1f cm of surveyed scene sits BEHIND the camera plane on %s (camera at %.1f, scene reaches %.1f) and is clipped out of every tile: the scene-capture orthographic projection hardcodes its near plane to 0. Move cameraDepthCm past the scene bounds, or omit it and let the camera be derived."),
            ContentBehindCameraCm, AxisName(DepthAxis), CameraDepthCm,
            (ForwardOnDepthAxis < 0.0) ? Survey.DepthMax : Survey.DepthMin)));
    }
    if (BlankTiles > 0)
    {
        Warnings.Add(MakeShared<FJsonValueString>(FString::Printf(
            TEXT("%d of %d tiles read back all-zero in every channel - the signature of a surface that was never drawn, not of a dark frame. They were written anyway and are flagged per tile as `blank`."),
            BlankTiles, TilesWritten)));
    }
    // The all-zero test above cannot see a frame the tonemapper filled with dither over nothing,
    // so the luminance the burst actually measured gets its own warning. A threshold, not a
    // verdict: the numbers are in `imageStats` and per tile either way.
    const double MeanLuminance = TilesWritten > 0 ? BurstMeanLuminance / TilesWritten : 0.0;
    if (TilesWritten > 0 && BurstMaxLuminance < 0.02)
    {
        Warnings.Add(MakeShared<FJsonValueString>(FString::Printf(
            TEXT("Every tile is essentially black: burst mean luminance %.5f, brightest pixel in the whole burst %.5f. Nothing read back all-zero, so this is not a blank readback - it is a frame drawn over nothing. Check that the extent covers content, that cameraDepthCm clears it on the right side (the near plane is 0), and that the exposure suits the scene; a headless editor also renders several stops darker than an interactive one."),
            MeanLuminance, BurstMaxLuminance)));
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("outputDir"), OutputDir);
    Result->SetStringField(TEXT("namePrefix"), Prefix);
    Result->SetStringField(TEXT("manifest"), ManifestPath);
    Result->SetStringField(TEXT("renderer"), PinWrightCaptureRenderer::SceneCapture2D);
    // Same key as the manifest above and as every other capture verb; see the note there.
    Result->SetStringField(TEXT("projectionMode"), TEXT("orthographic"));
    Result->SetNumberField(TEXT("tilesRequested"), static_cast<double>(TilesRequested));
    Result->SetNumberField(TEXT("tilesWritten"), TilesWritten);
    Result->SetNumberField(TEXT("blankTiles"), BlankTiles);
    Result->SetNumberField(TEXT("tilePixels"), TilePixels);
    Result->SetNumberField(TEXT("cmPerPixel"), PlannedCmPerPixel);
    Result->SetObjectField(TEXT("georeference"), SerializeGeoreference(Geo));
    Result->SetObjectField(TEXT("grid"), DescribeGrid(Geo));
    Result->SetObjectField(TEXT("camera"), CameraObj);
    Result->SetObjectField(TEXT("exposure"), ExposureObj);
    Result->SetObjectField(TEXT("showFlags"), Capture.DescribeShowFlags());
    Result->SetObjectField(TEXT("grass"), GrassObj);
    Result->SetObjectField(TEXT("timing"), TimingObj);
    Result->SetObjectField(TEXT("imageStats"), ImageObj);
    Result->SetObjectField(TEXT("level"), LevelObj);
    Result->SetBoolField(TEXT("extentExpanded"), bExtentExpanded);
    Result->SetObjectField(TEXT("requestedWorldMin"), MakeVectorObject(RequestedMin));
    Result->SetObjectField(TEXT("requestedWorldMax"), MakeVectorObject(RequestedMax));
    Result->SetBoolField(TEXT("tilesInResponse"), TileEntries.Num() <= PwOrthoMaxTilesInResponse);
    if (TileEntries.Num() <= PwOrthoMaxTilesInResponse)
    {
        Result->SetArrayField(TEXT("tiles"), TileEntries);
    }
    if (Stale.Num() > 0)
    {
        Result->SetArrayField(TEXT("staleFiles"), Stale);
    }
    Result->SetNumberField(TEXT("staleFileCount"), Stale.Num());
    if (Warnings.Num() > 0)
    {
        Result->SetArrayField(TEXT("warnings"), Warnings);
    }

    // An entirely blank burst is a broken render reported as a picture, which is the defect class
    // this plugin pays for most. The files and the manifest stay on disk and the whole measured
    // response travels with the error, so a caller who meant it can re-run with allowBlank.
    if (BlankTiles == TilesWritten && TilesWritten > 0 && !bAllowBlank)
    {
        Ctx.SendError(ErrorCodes::ERR_BLANK_CAPTURE,
            FString::Printf(
                TEXT("All %d tiles read back all-zero in every channel. The tiles and manifest were written to %s, but nothing was drawn into them - a headless editor with no RHI surface, a camera plane that clips the whole scene, or a level with nothing visible in this extent. Pass allowBlank:true to accept it."),
                TilesWritten, *OutputDir),
            Result);
        return true;
    }

    Ctx.SendSuccess(FString::Printf(
        TEXT("Rendered %d of %lld orthographic tiles (%d x %d at %d px, %.4f cm/px) to %s in %.1f s"),
        TilesWritten, TilesRequested, Grid.Cols, Grid.Rows, TilePixels, PlannedCmPerPixel,
        *OutputDir, BurstSeconds), Result);
    return true;
}
