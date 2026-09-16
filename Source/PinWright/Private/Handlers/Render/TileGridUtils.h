// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"

// THE georeference + tile-grid contract for the reference-image workflow. Every verb that maps
// between world centimetres and image pixels - the orthographic tile capture, the tiler, the
// annotator, the comparer - goes through this file and nothing else.
//
// WHY ONE COPY. Two implementations of this maths is the specific failure mode this header exists
// to prevent: the capture would place cameras by one convention and the reader would measure
// pixels by another, and the disagreement is invisible until a coordinate is ~1000 uu wrong. The
// same rule already governs ViewProjectionUtils.h (one ApplyCaptureCamera, so deprojected rays
// cannot drift off captured pixels) and PreviewViewportCaptureUtils.h (one DefaultOrthoWorldWidth,
// so an omitted orthoWidth always reconstructs the same view). Measured motivation: an eyeballed
// coordinate on a whole-map reference was off by ~1188 map units; chunking to native resolution
// improved it ~10x, which is only worth anything if the chunk->world mapping is exact.
//
// Pure: no UObject, no editor, no RHI, no Slate. Deterministic - identical input always yields
// identical output - so every claim below is unit-testable without an editor.
//
// ---- THE AXIS-MAPPING TRAP ----
//
// For an ORTHOGRAPHIC capture the engine IGNORES the camera rotation: FEditorViewportClient::
// CalcSceneView derives the view matrix from ELevelViewportType, and ResolveOrthographicView
// (PreviewViewportCaptureUtils.h) rejects any pose more than OrthoAxisToleranceDegrees off a
// cardinal world axis. So which world axis lands on screen X and which on screen Y is decided by
// the EFFECTIVE rotation the capture reports (FViewportCaptureOutput::EffectiveRotation) - never
// by the rotation that was requested. ActorLabelOverlay.h states the same rule for its
// behind-camera test, and for the same reason: a requested top-down yaw of 0 renders as a
// different yaw, so taking .Vector() off the request flips the frame.
//
// The mapping is therefore an EXPLICIT FIELD of the georeference (FScreenAxisMapping), not an
// assumption baked into the maths. Empirically recorded in docs/wiki-src/level-review.framing-math.md:
// pitch -90 / yaw 0 gives +X UP and +Y RIGHT; pitch -90 / yaw -90 gives +X RIGHT and +Y DOWN.
// TryMakeAxisMappingFromEffectiveRotation derives the same two answers from the rotation basis
// (screen right = the rotation's Y axis, screen up = its Z axis), and both are asserted in
// PinWright.render.tile_grid.AxisMapping* so the derivation cannot drift from the measurement.
//
// ---- OTHER ENGINE FACTS THIS ENCODES ----
//
//  * There is NO orthoHeight. The vertical world span of a frame is OrthoWidth * Height / Width
//    (PreviewViewportCaptureUtils.h: OrthoWidth is world centimetres left-to-right). A tile grid
//    whose two axes have different world-units-per-pixel therefore cannot be captured
//    orthographically at all, and ComputeTileCapturePlan refuses it rather than emitting a plan
//    that would render the wrong world span.
//  * An orthographic frame is centred on the camera POSITION, not on a look-at point
//    (CameraShotPlanUtils.h:92-95). The per-tile camera is placed at the CENTRE of the tile's
//    world sub-extent, with the depth axis supplied by the caller.
//
// ---- PIXEL CONVENTION ----
//
// TOP-LEFT origin, matching ViewProjectionUtils and every capture readback in the plugin.
// Coordinates are CONTINUOUS: pixel (0,0) covers [0,1) x [0,1), so the CENTRE of integer pixel
// (i,j) is (i + 0.5, j + 0.5). WorldToPixel/PixelToWorld are exact inverses on continuous
// coordinates; PixelCentreToWorld is the integer-pixel convenience built on top.
namespace PinWrightTileGrid
{
    // ---------------------------------------------------------------------------------------
    // Axis mapping
    // ---------------------------------------------------------------------------------------

    enum class EWorldAxis : uint8
    {
        X = 0,
        Y = 1,
        Z = 2
    };

    // Reads / writes one component of a world vector by axis. Free functions rather than a
    // switch at every call site, so a new axis cannot be handled inconsistently in one place.
    double GetAxisValue(const FVector& WorldPoint, EWorldAxis Axis);
    void SetAxisValue(FVector& WorldPoint, EWorldAxis Axis, double Value);
    const TCHAR* AxisName(EWorldAxis Axis);

    // Which world axis each SCREEN axis runs along, and in which direction.
    //
    // bScreenXPositive == true  means increasing that world axis moves RIGHT in the image.
    // bScreenYPositive == true  means increasing that world axis moves DOWN in the image.
    //
    // MUST describe the pose the PIXELS show. For an orthographic capture that is
    // FViewportCaptureOutput::EffectiveRotation, not the requested rotation (see the header
    // note above). There is no default that is right for both documented top-down poses, which
    // is why the presets are named after the poses instead of one being the default.
    struct FScreenAxisMapping
    {
        EWorldAxis ScreenXAxis = EWorldAxis::Y;
        bool bScreenXPositive = true;
        EWorldAxis ScreenYAxis = EWorldAxis::X;
        bool bScreenYPositive = false;

        // Two DIFFERENT world axes. A mapping that puts one axis on both screen axes is
        // degenerate: it is not invertible, so every pixel would deproject to a line.
        bool IsValid() const
        {
            return ScreenXAxis != ScreenYAxis;
        }

        // The world axis perpendicular to the capture plane - the one the caller must supply a
        // coordinate for when going back from pixels to a world point. Meaningless (returns X)
        // on an invalid mapping.
        EWorldAxis DepthAxis() const;

        bool operator==(const FScreenAxisMapping& Other) const
        {
            return ScreenXAxis == Other.ScreenXAxis && bScreenXPositive == Other.bScreenXPositive &&
                ScreenYAxis == Other.ScreenYAxis && bScreenYPositive == Other.bScreenYPositive;
        }
        bool operator!=(const FScreenAxisMapping& Other) const { return !(*this == Other); }
    };

    // Top-down, +X UP and +Y RIGHT. The pose at pitch -90 / yaw 0.
    FScreenAxisMapping TopDownXUpYRight();
    // Top-down, +X RIGHT and +Y DOWN. The pose at pitch -90 / yaw -90 - the one whose screen
    // axes read like a conventional map, and the reason the wiki tells callers to verify which
    // yaw they actually captured with.
    FScreenAxisMapping TopDownXRightYDown();
    // Looking along -X (the editor's "front" ortho view): +Y RIGHT, +Z UP.
    FScreenAxisMapping FrontYRightZUp();
    // Looking along -Y (the editor's "left"/"side" ortho view): +X RIGHT, +Z UP.
    FScreenAxisMapping SideXRightZUp();

    // Derive the mapping from the EFFECTIVE rotation a capture reported.
    //
    // Screen right is the rotation's Y basis vector and screen up is its Z basis vector, so this
    // is a derivation, not a guess - but it is only defined when BOTH land on a cardinal world
    // axis within Tolerance. A tilted pose has no axis mapping at all, and this returns false
    // with OutErr set rather than snapping to the nearest one: snapping is exactly the kind of
    // quiet wrongness that produces a plausible, mirrored map.
    //
    // Pass FViewportCaptureOutput::EffectiveRotation. Passing the REQUESTED rotation of an
    // orthographic capture is the documented way to get a mirrored georeference.
    bool TryMakeAxisMappingFromEffectiveRotation(const FRotator& EffectiveRotation,
        FScreenAxisMapping& OutMapping, FString& OutErr, double Tolerance = 1.0e-3);

    // Unit world direction a camera must LOOK ALONG to render Mapping's screen axes.
    //
    // Derived, never tabulated: screen right IS the rotation's Y basis and screen up IS the
    // negated screen-down direction (screen Y grows downward under the top-left pixel
    // convention) - the same two facts TryMakeAxisMappingFromEffectiveRotation reads in the
    // opposite direction - and UE's camera basis satisfies Forward = Right x Up. Returns the
    // zero vector on a degenerate mapping.
    //
    // The SIGN of this vector on the depth axis is what decides which side of the scene an
    // orthographic camera has to sit on, because the scene-capture ortho projection hardcodes
    // its near plane to 0 (Renderer/Private/SceneCaptureRendering.cpp BuildOrthoMatrix) and
    // clips everything behind the camera plane outright.
    FVector CameraForwardForAxisMapping(const FScreenAxisMapping& Mapping);

    // Camera rotation whose rendered frame has Mapping's screen axes. The inverse of
    // TryMakeAxisMappingFromEffectiveRotation, and the pose an orthographic tile capture places
    // its camera at.
    //
    // VERIFIED rather than asserted: the rotator is built from the basis above and then fed
    // straight back through TryMakeAxisMappingFromEffectiveRotation, and this returns false when
    // the round trip does not reproduce Mapping exactly. The two directions are independent code
    // (basis construction vs. matrix classification), so a sign error in either one is a refusal
    // here instead of a mirrored georeference that looks plausible on screen.
    bool TryMakeCameraRotationForAxisMapping(const FScreenAxisMapping& Mapping,
        FRotator& OutRotation, FString& OutErr);

    // ---------------------------------------------------------------------------------------
    // World extent
    // ---------------------------------------------------------------------------------------

    // A 2D axis-aligned world box in the capture plane, world CENTIMETRES.
    //
    // Component .X is the world axis mapped to SCREEN X; component .Y is the world axis mapped to
    // SCREEN Y (per the FScreenAxisMapping this extent is paired with). Min/Max are WORLD-ordered
    // (Min <= Max on each component) whatever the screen signs are - the sign lives in the
    // mapping and is applied in exactly one place, so a flipped axis cannot be encoded twice and
    // cancel itself out.
    struct FWorldExtent2D
    {
        FVector2D Min = FVector2D::ZeroVector;
        FVector2D Max = FVector2D::ZeroVector;

        // World span along the screen-X axis / the screen-Y axis, in centimetres.
        double SpanAcross() const { return Max.X - Min.X; }
        double SpanDown() const { return Max.Y - Min.Y; }
        FVector2D Centre() const { return FVector2D((Min.X + Max.X) * 0.5, (Min.Y + Max.Y) * 0.5); }

        // Strictly positive on both axes. A zero-span extent divides by zero in the pixel map,
        // so it is a failure, not an edge case: the default-constructed value is invalid.
        bool IsValid() const { return SpanAcross() > 0.0 && SpanDown() > 0.0; }
    };

    FWorldExtent2D MakeWorldExtent(double MinAcross, double MinDown, double MaxAcross, double MaxDown);

    // In-plane extent of a world-space AABB under Mapping, dropping the depth axis. Handles a
    // BoundsMin/BoundsMax pair given in either order per component.
    FWorldExtent2D MakeWorldExtentFromBounds(const FVector& BoundsMin, const FVector& BoundsMax,
        const FScreenAxisMapping& Mapping);

    // ---------------------------------------------------------------------------------------
    // The grid
    // ---------------------------------------------------------------------------------------

    // Zero-value is INVALID (Col/Row = INDEX_NONE), so a default-constructed index cannot be
    // mistaken for tile (0,0).
    struct FTileIndex
    {
        int32 Col = INDEX_NONE;
        int32 Row = INDEX_NONE;

        FTileIndex() = default;
        FTileIndex(int32 InCol, int32 InRow) : Col(InCol), Row(InRow) {}

        bool IsSet() const { return Col >= 0 && Row >= 0; }
        bool operator==(const FTileIndex& Other) const { return Col == Other.Col && Row == Other.Row; }
        bool operator!=(const FTileIndex& Other) const { return !(*this == Other); }
    };

    // World extent + axis mapping + a Cols x Rows subdivision at a FIXED tile pixel size.
    //
    // The full mosaic image size is DERIVED (Cols * TilePixelWidth, Rows * TilePixelHeight), not
    // stored, so a partial edge tile is unrepresentable. That is deliberate: an edge tile of a
    // different pixel size would need a different orthoWidth to capture, and the two would then
    // disagree about world-units-per-pixel at exactly the seam a reader measures across. Use
    // PlanTileGrid when you have a target resolution instead of a tile count.
    struct FTileGrid
    {
        FWorldExtent2D World;
        FScreenAxisMapping Axes;
        int32 Cols = 0;
        int32 Rows = 0;
        int32 TilePixelWidth = 0;
        int32 TilePixelHeight = 0;

        int32 ImageWidth() const { return Cols * TilePixelWidth; }
        int32 ImageHeight() const { return Rows * TilePixelHeight; }
        int64 TileCount() const { return static_cast<int64>(Cols) * static_cast<int64>(Rows); }

        // World centimetres per pixel along each screen axis. Zero on an invalid grid.
        double WorldUnitsPerPixelAcross() const;
        double WorldUnitsPerPixelDown() const;

        // The two scales agree to within RelativeTolerance. Required for orthographic capture:
        // there is no orthoHeight, so the vertical span is forced to OrthoWidth * H / W.
        bool HasSquarePixels(double RelativeTolerance = 1.0e-6) const;

        bool IsValid() const;
    };

    // Fills OutErr with the FIRST reason the grid cannot be used. Every entry point that takes a
    // grid runs this, so one definition of "usable" is shared by the capture and the readers.
    bool ValidateTileGrid(const FTileGrid& Grid, FString& OutErr);

    bool MakeTileGrid(const FWorldExtent2D& World, const FScreenAxisMapping& Mapping,
        int32 Cols, int32 Rows, int32 TilePixelWidth, int32 TilePixelHeight,
        FTileGrid& OutGrid, FString& OutErr);

    // A grid built to hit a resolution, plus what had to move to get there.
    struct FTileGridPlan
    {
        FTileGrid Grid;
        // The extent the caller asked for, kept so the plan can report the expansion rather than
        // silently returning a different area than was requested.
        FWorldExtent2D RequestedWorld;
        // Grid.World is strictly larger than RequestedWorld on at least one axis, because whole
        // tiles at the requested scale do not divide the requested span exactly.
        bool bExtentExpanded = false;
        // Scale actually achieved. Equals the requested target exactly - the planner absorbs the
        // remainder by expanding the extent, never by coarsening the scale.
        double WorldUnitsPerPixel = 0.0;
    };

    // Hard structural ceiling on either axis of a PLANNED grid. Not a taste limit: Cols and Rows
    // are int32 derived from a division, so an absurd span / scale ratio would overflow the
    // rounding before MaxTiles could refuse it.
    constexpr int32 MaxTilesPerAxis = 4096;

    // Choose Cols/Rows so the grid covers RequestedWorld at no coarser than
    // TargetWorldUnitsPerPixel, then EXPAND the extent symmetrically about its centre so the
    // covered area is exactly Cols x Rows whole tiles at that scale.
    //
    // The scale is uniform on both axes (square pixels) because that is the only thing an
    // orthographic capture can produce. Expansion, not cropping: cropping would silently drop
    // world the caller asked to see.
    //
    // MaxTiles <= 0 means uncapped. When the cap binds this returns false with OutErr naming the
    // required and permitted tile counts - a refusal, not a silently coarser grid, because a
    // coarser grid is precisely the measurement error this workflow exists to remove.
    bool PlanTileGrid(const FWorldExtent2D& RequestedWorld, const FScreenAxisMapping& Mapping,
        int32 TilePixelWidth, int32 TilePixelHeight, double TargetWorldUnitsPerPixel,
        int32 MaxTiles, FTileGridPlan& OutPlan, FString& OutErr);

    // ---------------------------------------------------------------------------------------
    // world <-> pixel
    // ---------------------------------------------------------------------------------------

    // Continuous pixel coordinate in the FULL mosaic, TOP-LEFT origin. Points outside the extent
    // project outside [0, ImageWidth] x [0, ImageHeight] rather than being clamped - clamping
    // would report an in-frame coordinate for something that is not in frame. Returns
    // (0,0) on an invalid grid; callers that can act on the difference should ValidateTileGrid first.
    FVector2D WorldToPixel(const FTileGrid& Grid, const FVector& WorldPoint);

    // Inverse of WorldToPixel on continuous coordinates. DepthCoordinate fills the component on
    // Grid.Axes.DepthAxis() - the capture plane carries no depth information, so the caller must
    // supply it (typically the plane being measured, e.g. Z = 0 for a ground-plane read).
    FVector PixelToWorld(const FTileGrid& Grid, const FVector2D& Pixel, double DepthCoordinate);

    // World point at the CENTRE of integer pixel (PixelX, PixelY) - i.e. PixelToWorld at
    // (PixelX + 0.5, PixelY + 0.5). This is what a reader wants when they have picked a pixel
    // off an image; PixelToWorld on the bare integer returns the pixel's top-left CORNER.
    FVector PixelCentreToWorld(const FTileGrid& Grid, int32 PixelX, int32 PixelY, double DepthCoordinate);

    // Inside the mosaic, edges INCLUSIVE ([0, ImageWidth] x [0, ImageHeight]). Inclusive because
    // the far edge is a real world coordinate - the extent's Max corner lands exactly there.
    bool IsPixelInsideImage(const FTileGrid& Grid, const FVector2D& Pixel);

    // ---------------------------------------------------------------------------------------
    // world <-> (tile, pixel-within-tile)
    // ---------------------------------------------------------------------------------------

    struct FTilePixel
    {
        FTileIndex Tile;
        FVector2D PixelInTile = FVector2D::ZeroVector;
    };

    // Split a mosaic pixel into (tile, pixel-within-tile).
    //
    // SEAM RULE: half-open per tile. A coordinate exactly on a seam (an exact multiple of the
    // tile pixel size) belongs to the tile on its RIGHT / BELOW, at PixelInTile 0 - it lands in
    // exactly ONE tile, never both, and never neither. The single exception is the far edge of
    // the whole image, which has no tile to its right: it is assigned to the LAST tile at
    // PixelInTile == TilePixelWidth/Height, so the extent's Max corner is addressable.
    //
    // Returns false (leaving Out.Tile unset) for a pixel outside the image.
    bool PixelToTilePixel(const FTileGrid& Grid, const FVector2D& Pixel, FTilePixel& Out);

    // Inverse: mosaic pixel for a pixel-within-tile. Does not validate that PixelInTile is inside
    // the tile - a negative or over-size value is a legitimate way to address a neighbouring
    // tile's pixels in the mosaic frame.
    FVector2D TilePixelToPixel(const FTileGrid& Grid, const FTileIndex& Tile, const FVector2D& PixelInTile);

    bool WorldToTilePixel(const FTileGrid& Grid, const FVector& WorldPoint, FTilePixel& Out);
    FVector TilePixelToWorld(const FTileGrid& Grid, const FTileIndex& Tile, const FVector2D& PixelInTile,
        double DepthCoordinate);

    // World sub-extent covered by one tile. False when Tile is outside the grid.
    bool TileWorldExtent(const FTileGrid& Grid, const FTileIndex& Tile, FWorldExtent2D& OutExtent);

    // ---------------------------------------------------------------------------------------
    // Per-tile capture
    // ---------------------------------------------------------------------------------------

    // Everything render.capture_ortho_tiles needs to render ONE tile, and nothing it can derive
    // wrongly on its own.
    struct FTileCapturePlan
    {
        FTileIndex Tile;
        FWorldExtent2D World;
        // Ortho frames are centred on the camera POSITION, not a look-at point
        // (CameraShotPlanUtils.h:92-95), so this IS the centre of World, with the depth axis set
        // to the caller's DepthCoordinate.
        FVector CameraLocation = FVector::ZeroVector;
        // World centimetres spanned left-to-right, straight into FViewportCaptureRequest::OrthoWidth.
        // There is no OrthoHeight: the vertical span is OrthoWidth * PixelHeight / PixelWidth,
        // which equals World.SpanDown() only because the grid has square pixels (enforced below).
        float OrthoWidth = 0.0f;
        int32 PixelWidth = 0;
        int32 PixelHeight = 0;
    };

    // Refuses (false + OutErr) on an invalid grid, an out-of-range tile, or a grid whose pixels
    // are not square. The square-pixel refusal is not pedantry: an ortho capture has only
    // OrthoWidth, so a non-square grid would be rendered at the wrong vertical world span while
    // every pixel<->world call in this header kept reporting the grid's own scale.
    bool ComputeTileCapturePlan(const FTileGrid& Grid, const FTileIndex& Tile, double DepthCoordinate,
        FTileCapturePlan& OutPlan, FString& OutErr);

    // ---------------------------------------------------------------------------------------
    // Snapping
    // ---------------------------------------------------------------------------------------

    // The map work this was built for snaps to a 100 uu grid. It is a DEFAULT offered to callers,
    // never applied implicitly - every snap function takes its grid size as an argument.
    constexpr double DefaultWorldSnapGridCm = 100.0;

    // Nearest multiple of GridSize, ties resolved toward +infinity. Identical to FMath::GridSnap
    // (floor(Value / GridSize + 0.5) * GridSize) so plugin code and engine code cannot disagree
    // about where a snapped coordinate lands; spelled out here because the negative branch is the
    // one that goes wrong (truncation toward zero maps -150 and 150 onto the same distance from
    // their neighbours, floor does not: -150 -> -100, 150 -> 200).
    //
    // GridSize <= 0 returns Value unchanged - there is no meaningful snap to a zero grid, and a
    // verb taking a grid size from a caller must reject it before getting here.
    double SnapToWorldGrid(double Value, double GridSize);
    FVector2D SnapToWorldGrid(const FVector2D& Value, double GridSize);
    FVector SnapToWorldGrid(const FVector& Value, double GridSize);
}
