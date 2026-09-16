// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#include "Handlers/Render/BitmapPaint.h"
#include "Handlers/Render/TileGridUtils.h"

// The file-level half of the `image.*` namespace: decode / slice / composite / encode, plus the
// ONE parser and ONE serializer for the georeference that travels on the wire.
//
// WHY THIS EXISTS. `image.tile`, `image.annotate` and `image.compare` all have to answer the same
// question - "which world centimetres does this pixel cover?" - and the whole point of the feature
// is that the caller never answers it by hand. The maths itself is NOT here: every world<->pixel
// conversion goes through PinWrightTileGrid, and every painted pixel goes through
// PinWrightBitmapPaint. What lives here is the wire representation and the file I/O around them, so
// the three verbs cannot drift in how they read a georeference or how they cut a mosaic.
//
// Pure with respect to the editor: no UObject, no world, no viewport, no RHI, no Slate. It touches
// the filesystem and the CPU and nothing else, which is why none of the three verbs is tick-unsafe.
//
// ---- THE GEOREFERENCE IS RESOLUTION-FREE, DELIBERATELY ----
//
// The wire georeference carries axes + world AABB + cols/rows + depth. It does NOT carry a tile
// pixel size: the pixel size is always DERIVED from the image actually being processed. That is
// what makes "reference tile (r,c) and capture tile (r,c) are the same ground" true across two
// different resolutions - which is the normal case, because a downloaded reference and an
// orthographic capture almost never agree on pixel count. A georeference that carried a pixel size
// would have to be edited to be reused, and an edited georeference is an unverifiable one.
//
// The consequence the caller sees: a mosaic whose width is not an exact multiple of `cols` is
// REFUSED (IMAGE_GRID_MISMATCH) rather than rounded. A partial edge tile is unrepresentable in
// FTileGrid (see TileGridUtils.h), and rounding it away would move the seam the reader measures
// across.
namespace PinWrightImage
{
    // Structural ceiling on one call's tile burst. Not a taste limit: every tile is a separate
    // decode + encode + file write, so an unbounded grid is an unobservable multi-minute wedge on
    // the game thread with no job handle to cancel it. A caller that genuinely needs more cuts the
    // work into several calls, and sees that it is doing so.
    constexpr int32 MaxTilesPerCall = 1024;

    // Ceiling on grid lines drawn per screen axis by image.annotate. A spacing that produces more
    // is refused (GRID_TOO_DENSE) rather than painted: past roughly one line per pixel the overlay
    // IS the image, which is the full-opacity failure in a different disguise.
    constexpr int32 MaxGridLinesPerAxis = 512;

    // ---------------------------------------------------------------------------------------
    // Bitmaps
    // ---------------------------------------------------------------------------------------

    // An owned BGRA8 image. FColor is laid out B,G,R,A in memory, which is what both
    // FImageUtils::LoadImage(..., ERawImageFormat::BGRA8) hands back and what
    // PinWrightScreenshotUtils::EncodeBitmapToPng expects, so no channel swizzle happens anywhere
    // in this file.
    struct FBitmap
    {
        TArray<FColor> Pixels;
        int32 Width = 0;
        int32 Height = 0;

        // EXACT pixel count, not "at least": a bitmap that owns its storage has no reason to be
        // over-long, and an over-long one usually means two decodes were mixed up.
        bool IsValid() const
        {
            return Width > 0 && Height > 0 &&
                static_cast<int64>(Pixels.Num()) == static_cast<int64>(Width) * static_cast<int64>(Height);
        }

        PinWrightBitmapPaint::FSurface Surface()
        {
            return PinWrightBitmapPaint::FSurface(TArrayView<FColor>(Pixels), Width, Height);
        }
    };

    // Decode any format FImageUtils understands into BGRA8/sRGB. Fills a TYPED error code
    // (FILE_NOT_FOUND / DECODE_FAILED) so the handler never has to invent one.
    bool LoadBitmap(const FString& AbsolutePath, FBitmap& Out, FString& OutErrCode, FString& OutErrMsg);

    // Encode as PNG and write. Creates the containing directory. ENCODE_FAILED / WRITE_FAILED.
    bool SaveBitmapPng(const FString& AbsolutePath, const FBitmap& In, FString& OutErrCode, FString& OutErrMsg);

    bool MakeBitmap(int32 Width, int32 Height, const FColor& Fill, FBitmap& Out, FString& OutErr);

    // Half-open copy of [OriginX, OriginX + W) x [OriginY, OriginY + H). Refuses (false) rather
    // than clamping when the rect leaves the source: a silently short tile is a wrong tile.
    bool CopyRegion(const FBitmap& Src, int32 OriginX, int32 OriginY, int32 W, int32 H,
        FBitmap& Out, FString& OutErr);

    // Opaque copy of Src into Dst at (OriginX, OriginY). Refuses when Src would leave Dst.
    bool BlitInto(FBitmap& Dst, const FBitmap& Src, int32 OriginX, int32 OriginY, FString& OutErr);

    // Cut one tile out of a mosaic.
    //
    // The origin comes from PinWrightTileGrid::TilePixelToPixel and the size from the grid's own
    // TilePixelWidth/Height, so the slice CANNOT disagree with the georeference - the seam rule is
    // whatever TileGridUtils says it is, not a second copy of it. The slice is half-open to match
    // PixelToTilePixel, which assigns a seam coordinate to the tile on its right / below.
    bool ExtractTile(const FBitmap& Mosaic, const PinWrightTileGrid::FTileGrid& Grid,
        const PinWrightTileGrid::FTileIndex& Tile, FBitmap& Out, FString& OutErr);

    // ---------------------------------------------------------------------------------------
    // Georeference (the wire object)
    // ---------------------------------------------------------------------------------------

    // A parsed georeference plus everything derived from it once.
    struct FGeoreference
    {
        // Axes + in-plane world extent + cols/rows + the tile pixel size derived from the image.
        PinWrightTileGrid::FTileGrid Grid;
        // The 3D AABB exactly as the caller supplied it, kept so serialization round-trips
        // byte-for-byte rather than reconstructing a box from the in-plane extent (which has
        // dropped the depth axis).
        FVector WorldMin = FVector::ZeroVector;
        FVector WorldMax = FVector::ZeroVector;
        // World coordinate on Grid.Axes.DepthAxis() used for every pixel -> world answer this
        // georeference produces. Defaults to the midpoint of the AABB's depth span - a definition,
        // not a guess - and is reported on every response so it is never implicit.
        double DepthCm = 0.0;
        bool bDepthExplicit = false;
    };

    // Accepts either a preset name or the explicit object form
    // {screenXAxis, screenXPositive, screenYAxis, screenYPositive}. An unrecognized preset name is
    // an ERROR, never a fallback (rpc-design.md §3): a silently substituted mapping mirrors every
    // coordinate the caller reads off the image, and a mirrored map looks plausible.
    bool ParseAxes(const TSharedPtr<FJsonValue>& Value, PinWrightTileGrid::FScreenAxisMapping& Out,
        FString& OutErr);

    // The preset name for a mapping, or empty when it is not one of the four.
    FString AxesPresetName(const PinWrightTileGrid::FScreenAxisMapping& Mapping);
    // Preset name as a JSON string when there is one, else the explicit object. Round-trips
    // through ParseAxes either way.
    TSharedPtr<FJsonValue> SerializeAxes(const PinWrightTileGrid::FScreenAxisMapping& Mapping);
    // Always the explicit form, for the human-readable `grid` block.
    TSharedPtr<FJsonObject> DescribeAxes(const PinWrightTileGrid::FScreenAxisMapping& Mapping);

    // Parse the wire georeference against the image that is actually being processed.
    //
    // ImageWidth/ImageHeight are the dimensions of the FILE in hand. bImageIsSingleTile says which
    // thing that file is: one tile of the grid (annotate with `tile`), or the whole mosaic. The
    // tile pixel size is derived from that pair - it is never read off the wire.
    //
    // Unknown keys are rejected. The dispatcher's UNKNOWN_PARAMS check only sees TOP-LEVEL
    // parameters, so a typo inside a nested object would otherwise be silently ignored and change
    // the meaning of the call.
    bool ParseGeoreference(const TSharedPtr<FJsonObject>& Obj, int32 ImageWidth, int32 ImageHeight,
        bool bImageIsSingleTile, FGeoreference& Out, FString& OutErrCode, FString& OutErrMsg);

    // The exact object ParseGeoreference accepts back. Carries no pixel size by construction.
    TSharedPtr<FJsonObject> SerializeGeoreference(const FGeoreference& Geo);

    // Everything derived: pixel sizes, cm/px on each axis, the explicit axis mapping, the in-plane
    // extent with its axes named. Reported beside the georeference, never inside it.
    TSharedPtr<FJsonObject> DescribeGrid(const FGeoreference& Geo);

    // True when two georeferences describe the same ground: same axes, same world AABB corners in
    // the capture plane, same subdivision, same depth. Pixel sizes are deliberately NOT compared -
    // they are a property of the images, and image.compare checks those separately, so a genuine
    // cross-resolution pair fails on the dimension check with the right error rather than on this
    // one with the wrong one. OutWhy names the FIRST field that differs.
    bool GeoreferencesAgree(const FGeoreference& A, const FGeoreference& B, FString& OutWhy);

    // Pull a georeference object out of a file on disk: a manifest written by image.tile (root
    // `georeference`, or `tiles[i].georeference` when TileSelector is set), or a bare file whose
    // root IS a georeference. INVALID_MANIFEST / FILE_NOT_FOUND.
    bool LoadGeoreferenceObjectFromFile(const FString& AbsolutePath,
        const PinWrightTileGrid::FTileIndex& TileSelector, TSharedPtr<FJsonObject>& Out,
        FString& OutErrCode, FString& OutErrMsg);

    // EXACTLY one of the inline object and the file path. Neither is an error and both is an
    // error: silently preferring one would let a caller who supplied a stale `georeferenceFrom`
    // alongside a corrected inline object measure against the stale one and never know.
    bool ResolveGeoreferenceObject(const TSharedPtr<FJsonObject>& InlineObject,
        const FString& FromPath, const PinWrightTileGrid::FTileIndex& TileSelector,
        TSharedPtr<FJsonObject>& Out, FString& OutErrCode, FString& OutErrMsg);

    // ---------------------------------------------------------------------------------------
    // Small shared parsers / builders
    // ---------------------------------------------------------------------------------------

    // "#RRGGBB", "#RRGGBBAA", "RRGGBB", or one of the named colours. Unknown text is an error.
    bool ParseColorString(const FString& Text, FColor& Out, FString& OutErr);

    // {x, y, z} or [x, y, z]. Z defaults to 0 when absent, because the capture plane carries no
    // depth of its own; every in-plane read ignores it anyway.
    bool ParseWorldPoint(const TSharedPtr<FJsonValue>& Value, FVector& Out, FString& OutErr);
    bool ParseWorldPointField(const TSharedPtr<FJsonObject>& Obj, const FString& Field, FVector& Out,
        FString& OutErr);

    // Image-facing adapter around JsonUtils' shared collector. It preserves this namespace's
    // sorted multi-key error text for callers in the split image handler files.
    bool RejectUnknownKeys(const TSharedPtr<FJsonObject>& Obj, const TArray<FString>& Allowed,
        const FString& ContextName, FString& OutErr);

    TSharedPtr<FJsonObject> MakeVectorObject(const FVector& V);
    TSharedPtr<FJsonObject> MakePixelObject(const FVector2D& P);

    // ---------------------------------------------------------------------------------------
    // Paths
    // ---------------------------------------------------------------------------------------

    // Absolute path for a caller-supplied input file: absolute stays put, relative resolves
    // against the project directory.
    FString ResolveInputPath(const FString& Requested);

    // Where a burst lands. RequestedDir wins when supplied (the ui.screenshot precedent - see
    // Utils/ScreenshotUtils.h - because a reference image and its tiles belong beside each other,
    // wherever the caller keeps them). Otherwise <ProjectSaved>/PinWright/image/<DefaultSubdir>.
    //
    // Deliberately NOT PinWrightScreenshotUtils::MakeScreenshotOutputPath, which forces
    // Saved/Screenshots/<Subdir>: that directory is where every capture verb drops single frames
    // for a human to scan, and a 64-file tile burst into it destroys that use. The default root is
    // the plugin's own Saved tree, beside Saved/PinWright/wiki and the asset dumps.
    FString ResolveOutputDir(const FString& RequestedDir, const FString& DefaultSubdir);

    // Path-traversal-safe basename with no extension. Falls back to Fallback when the request is
    // empty or is nothing but separators.
    FString SanitizeBaseName(const FString& Requested, const FString& Fallback);
}
