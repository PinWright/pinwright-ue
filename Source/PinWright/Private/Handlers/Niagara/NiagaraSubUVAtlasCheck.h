// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"

#include "Handlers/Asset/TexturePixelStats.h"

class UNiagaraRendererProperties;
class UNiagaraSystem;

namespace PinWrightNiagara
{
    // Where the texture a renderer's SubUV grid was checked against came from. Published so a
    // caller can judge how much a finding is worth: a renderer-level binding is the texture the
    // vertex factory really slices, while "the material samples exactly one texture" is an
    // inference that is right on a flipbook material and wrong on a multi-sampler one.
    enum class ESubUVTextureSource : uint8
    {
        Unresolved,
        // The renderer's own MaterialParameters.TextureParameters entry. A MID is built from it
        // per emitter instance, so it overrides whatever the material asset samples.
        RendererTextureParameter,
        // A ParticleSubUV / TextureSampleParameterSubUV expression in the material graph - the
        // node that exists to declare which texture is the flipbook.
        MaterialSubUVExpression,
        // The material samples exactly one texture, so which one is the atlas is not in doubt.
        MaterialSoleTextureSample
    };

    // One enabled sprite/mesh renderer that declares a SubUV grid, and what could be established
    // about the texture it slices.
    struct FSubUVRendererAtlasFinding
    {
        FString EmitterName;
        int32 RendererIndex = INDEX_NONE;
        FString RendererClass;
        // UNiagaraSpriteRendererProperties / UNiagaraMeshRendererProperties::SubImageSize.
        FIntPoint DeclaredSubImageSize = FIntPoint(1, 1);

        FString MaterialPath;
        FString TexturePath;
        ESubUVTextureSource TextureSource = ESubUVTextureSource::Unresolved;
        int32 TextureWidth = 0;
        int32 TextureHeight = 0;

        // Why the declared grid could not be measured against pixels at all, empty when it could.
        // Never left empty on an unmeasured renderer: silence there reads as "checked and fine".
        FString UnverifiedReason;

        // ---- Heuristics (weak; a disagreement here is a warning) ----

        // The texture's dimensions are not whole multiples of the declared grid, so the vertex
        // factory's 1/X x 1/Y window cannot line up with whole texels.
        bool bDimensionsIndivisible = false;
        // An NxM spelled in the texture's asset name (T_Smoke_8x8). (0,0) when the name spells none.
        FIntPoint NameEncodedGrid = FIntPoint::ZeroValue;

        // ---- Trailing-empty measurement (weak; a disagreement here is a warning) ----

        // The declared grid with its wholly-empty trailing columns and rows dropped. (0,0) when
        // the pixels could not be measured, or when every tile is blank. This NEVER proves a
        // different cell size: an 8x8 sheet with 56 authored frames, and an alpha-faded flipbook
        // whose last row is fully transparent, both legitimately leave a trailing row empty.
        FIntPoint PopulatedGrid = FIntPoint::ZeroValue;
        // Tiles of the declared grid carrying no pixel content at all, wherever they sit.
        int32 EmptyTileCount = 0;

        // ---- Gutter-period detection (the strong signal; a disagreement here is an error) ----

        // The grid read off the periodic minima of the row/column energy profiles: the cell size
        // the atlas is actually laid out at, independent of what SubImageSize claims. (0,0) when
        // no regular gutter period was readable on both axes.
        FIntPoint DetectedGrid = FIntPoint::ZeroValue;
        // 0..1 per axis: how far below the profile's own mean the darkest cut line sits. 1.0 means
        // every cell boundary is black.
        double DetectedColumnConfidence = 0.0;
        double DetectedRowConfidence = 0.0;
        // Why no period was readable, empty when one was. A gutterless atlas (frames drawn edge to
        // edge) is the ordinary reason and is not a defect.
        FString DetectionReason;

        bool HasDetectedGridMismatch() const { return DetectedGrid.X > 0 && DetectedGrid != DeclaredSubImageSize; }
        bool HasTrailingEmptyMismatch() const { return PopulatedGrid.X > 0 && PopulatedGrid != DeclaredSubImageSize; }
        bool HasNameMismatch() const { return NameEncodedGrid.X > 0 && NameEncodedGrid != DeclaredSubImageSize; }
        bool IsMismatch() const
        {
            return HasDetectedGridMismatch() || HasTrailingEmptyMismatch() || HasNameMismatch() || bDimensionsIndivisible;
        }
    };

    enum class ESubUVAtlasVerdict : uint8
    {
        // No enabled renderer declares a SubImageSize other than (1,1). Nothing to check.
        NotApplicable,
        // Every declared grid was checked against the texture it slices and agrees.
        Consistent,
        // At least one declared grid disagrees with its texture.
        Mismatched,
        // At least one renderer declares a grid whose texture could not be resolved or read.
        // NOT a pass - an empty finding list under this verdict is not evidence of soundness.
        Unverified
    };

    // Walks every enabled renderer of every enabled emitter handle and checks each declared
    // SubUV grid against the texture that renderer samples. OutFindings is reset on entry and
    // holds one entry per renderer that declares a grid, whatever the verdict.
    ESubUVAtlasVerdict EvaluateSubUVAtlases(const UNiagaraSystem& System, TArray<FSubUVRendererAtlasFinding>& OutFindings);

    // Single-renderer form. Returns false when the renderer declares no SubUV grid (SubImageSize
    // (1,1), or a class that has no such property), in which case OutFinding is meaningless.
    bool EvaluateRendererSubUVAtlas(const UNiagaraRendererProperties& Renderer, FSubUVRendererAtlasFinding& OutFinding);

    // The declared grid with its wholly-empty trailing columns and rows dropped, from per-tile
    // stats measured over that same grid (row-major, Columns*Rows entries). Returns (0,0) when
    // the input does not describe the grid, or when every tile is blank - a wholly empty texture
    // says nothing about its layout. OutEmptyTileCount receives the count of blank tiles wherever
    // they sit, which is reported even when none of them are trailing.
    FIntPoint DetectPopulatedGrid(const TArray<TexturePixelStats::FRegionStats>& Tiles,
        int32 Columns, int32 Rows, int32& OutEmptyTileCount);

    // How many cells an axis is divided into, read off the gutter period of a 1-D energy profile
    // (one entry per texel column, or per texel row, of the mip).
    //
    // Scores each candidate division by the BRIGHTEST sample landing on any of its interior cut
    // lines: a real grid puts every cut in a gutter, a wrong one drives at least one cut through
    // a frame. The LARGEST passing division is the answer - a divisor of the true grid also lands
    // its cuts in real gutters (a 2-way split of a 6-cell axis cuts at the third gutter), so
    // taking the smallest would report 2 for every atlas. The drawn slices must also form a
    // contiguous prefix of at least two, or "the cuts are dark" is just a mostly-empty image and
    // any division passes; trailing blank slices are allowed, because a flipbook holding fewer
    // frames than it has cells is ordinary and its true grid must still be readable.
    //
    // Returns 0 when no division qualifies, which is the ordinary answer for an atlas whose
    // frames are drawn edge to edge with no gutter. OutConfidence is 0..1.
    int32 DetectAxisDivisions(const TArray<double>& Profile, int32 MaxDivisions, double& OutConfidence);

    // An NxM grid spelled in an asset name ("T_Smoke_8x8", "Fire_subUV_6X6_02"). Only the last
    // such run is taken, both sides must be 1-2 digits in 1..64, and the run must not be part of
    // a longer number - so a 1024x1024 in a name reads as a pixel size and is ignored.
    bool ParseGridFromAssetName(const FString& AssetName, FIntPoint& OutGrid);

    // Stable wire spelling: "not_applicable" / "consistent" / "mismatched" / "unverified".
    const TCHAR* SubUVAtlasVerdictToString(ESubUVAtlasVerdict Verdict);

    // Caller-facing sentence naming the declared grid, the texture, and the evidence.
    FString DescribeSubUVAtlasFinding(const FSubUVRendererAtlasFinding& Finding);
}
