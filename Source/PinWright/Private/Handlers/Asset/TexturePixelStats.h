// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Engine/TextureDefines.h"

class UTexture;

// Live source-mip pixel-content readback for the pixel-mutating texture verbs
// (desaturate / invert / adjust_levels / channel_pack / combine_textures).
//
// Every texture *read* RPC (texture.describe / texture.get_texture_info) returns
// only metadata, so a caller cannot confirm a desaturate actually produced
// grayscale or an invert actually flipped the image — see
// F-texture-pixel-stats-readback. This builds cheap content-revealing aggregates
// from the editable source mip (Texture->Source.LockMip), the same access pattern
// the mutating verbs already use, so there is no streaming / platform-decode
// dependency.
//
// The aggregates are addressable below the whole image: a sub-rectangle, or a
// columns x rows tiling of one. A whole-image mean is close to meaningless on a
// tiled authoring texture — a SubUV flipbook atlas, a sprite sheet, a row-banded
// AO sheet — where every question is about one cell, and answering those outside
// the plugin costs a hand-written PNG decode over an editor thumbnail.
namespace TexturePixelStats
{
    // The byte-per-channel source layouts the pixel readers can decode: TSF_BGRA8 (B,G,R,A)
    // and single-channel TSF_G8. This is the one place the accepted-source-format policy
    // lives — any verb reading FTextureSource bytes must reject other formats (compressed /
    // float layouts would misread the byte order) rather than duplicate the allowlist.
    PINWRIGHT_API bool IsReadableSourceFormat(ETextureSourceFormat Format);

    // A rectangle inside one mip, in that mip's own pixel coordinates.
    struct FPixelRegion
    {
        int32 X = 0;
        int32 Y = 0;
        int32 Width = 0;
        int32 Height = 0;
    };

    // Aggregates over one rectangle of one mip.
    //
    // A single-channel (TSF_G8) source reports its gray value in all three of MeanR/G/B and
    // leaves alpha at 255, so a C++ caller measuring luminance layout reads one shape whatever
    // the source format is. Only the JSON writer cares about the {gray} wire spelling, and it
    // branches on bSingleChannel.
    struct FRegionStats
    {
        FPixelRegion Region;
        int64 PixelCount = 0;
        bool bSingleChannel = false;
        double MeanR = 0.0;
        double MeanG = 0.0;
        double MeanB = 0.0;
        double MeanA = 0.0;
        uint8 MinR = 0;
        uint8 MinG = 0;
        uint8 MinB = 0;
        uint8 MinA = 0;
        uint8 MaxR = 0;
        uint8 MaxG = 0;
        uint8 MaxB = 0;
        uint8 MaxA = 0;
        int32 MaxChannelSpread = 0;
        bool bGrayscale = true;

        // Unweighted channel mean. Deliberately not Rec.709: the callers that read this are
        // asking "is anything drawn here", not "how bright does this look", and a weighted
        // luma hides content that lives only in the blue channel.
        double MeanLuma() const { return (MeanR + MeanG + MeanB) / 3.0; }
    };

    struct FPixelStatsOptions
    {
        // Source mip level to read (0 = full-res).
        int32 MipIndex = 0;
        // When set, the stats cover Region instead of the whole mip. Region is clamped to the
        // mip; an origin outside the mip is an error rather than a silent empty read.
        bool bHasRegion = false;
        FPixelRegion Region;
        // When set, the covered area is split into TileColumns x TileRows tiles, each reported
        // with its own stats block, row-major (left-to-right, top-to-bottom). Carried as its
        // own flag so a caller asking for a 0-column grid is refused rather than silently
        // served the whole-image answer.
        bool bHasTileGrid = false;
        int32 TileColumns = 0;
        int32 TileRows = 0;
    };

    // Computes per-channel mean/min/max, a grayscale flag (+ maxChannelSpread), and a
    // content hash of the source mip for the given mip level. Returns a JSON object on
    // success, or nullptr with OutError populated (no source data, out-of-range mip, or
    // an unsupported source format). Only the source formats the texture verbs emit are
    // supported (TSF_BGRA8, TSF_G8); anything else fails loud rather than misreading the
    // byte layout. Takes a non-const UTexture because FTextureSource::LockMip is mutable.
    PINWRIGHT_API TSharedPtr<FJsonObject> BuildPixelStatsJson(UTexture* Texture, int32 MipIndex, FString& OutError);

    // Region / tile-grid form. With default options the response is byte-identical to the
    // two-argument overload's: `region` and the tile block are appended only when asked for,
    // and `hash` always covers the whole mip so its meaning never depends on the request.
    PINWRIGHT_API TSharedPtr<FJsonObject> BuildPixelStatsJson(UTexture* Texture, const FPixelStatsOptions& Options, FString& OutError);

    // Per-tile aggregates over a Columns x Rows grid covering the whole mip, row-major.
    // The C++ entry point behind the JSON tile block, for callers that measure a layout
    // rather than report one (niagara.validate's SubUV atlas check).
    PINWRIGHT_API bool MeasureTileGrid(UTexture* Texture, int32 MipIndex, int32 Columns, int32 Rows,
        TArray<FRegionStats>& OutTiles, FString& OutError);
}
