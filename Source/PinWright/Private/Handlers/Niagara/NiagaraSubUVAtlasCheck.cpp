// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/Niagara/NiagaraSubUVAtlasCheck.h"

#include "Engine/Texture.h"
#include "Engine/Texture2D.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionMaterialFunctionCall.h"
#include "Materials/MaterialExpressionParticleSubUV.h"
#include "Materials/MaterialExpressionTextureBase.h"
#include "Materials/MaterialExpressionTextureSampleParameterSubUV.h"
#include "Materials/MaterialFunctionInterface.h"
#include "Materials/MaterialInterface.h"
#include "NiagaraEmitter.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraMeshRendererProperties.h"
#include "NiagaraRendererProperties.h"
#include "NiagaraSpriteRendererProperties.h"
#include "NiagaraSystem.h"

namespace
{
    // A tile with no content at all, not a dim one. The peak channel is used rather than the
    // mean because one bright texel is enough to make a flipbook cell a real frame, and a
    // near-zero threshold keeps DXT ringing and dither noise from reading as content.
    constexpr int32 SubUVEmptyTilePeak = 2;

    // Material function graphs are walked, but not indefinitely: a cycle is impossible through
    // the visited set and this only bounds pathological nesting.
    constexpr int32 SubUVMaxFunctionDepth = 8;

    // Gutter detection thresholds, all on the 0-255 energy scale of the axis profiles.
    //
    // A cut line counts as landing in a gutter only when the profile there is essentially dark
    // relative to the sheet's own mean. Deliberately strict: this signal is the ERROR path, and
    // a loose threshold would turn "the frames nearly touch" into a reported defect.
    constexpr double SubUVGutterMaxMeanRatio = 0.10;
    // Every slice of a candidate division must carry something, or "the cuts are dark" is just a
    // mostly-empty image and every division passes it.
    constexpr double SubUVSliceContentRatio = 0.05;
    // Below this the axis carries no drawn content at all and no period can be read from it.
    constexpr double SubUVProfileFloor = 0.5;
    // Grids beyond this are not authored by hand; the ceiling also keeps the candidate scan cheap.
    constexpr int32 SubUVMaxDetectedDivisions = 64;
    // The profile is one entry per texel column (or row) up to this many. A wider mip is
    // sub-sampled into this many strips, which widens the effective gutter test rather than
    // breaking it.
    constexpr int32 SubUVMaxProfileSamples = 4096;

    bool IsBlankSubUVTile(const TexturePixelStats::FRegionStats& Tile)
    {
        const int32 PeakRgb = FMath::Max3<int32>(Tile.MaxR, Tile.MaxG, Tile.MaxB);
        // A fully transparent tile is blank however bright its RGB is: an alpha-masked atlas
        // commonly leaves white in the unused cells.
        return PeakRgb <= SubUVEmptyTilePeak || Tile.MaxA == 0;
    }

    // What a sprite built from this strip would actually put on screen: mean luminance scaled by
    // mean coverage. An additive grayscale flipbook carries its shape in luminance and an
    // alpha-masked one carries it in alpha with white RGB, and this reads both.
    double TileEnergy(const TexturePixelStats::FRegionStats& Tile)
    {
        return Tile.MeanLuma() * (Tile.MeanA / 255.0);
    }

    const FNiagaraRendererMaterialParameters* GetRendererMaterialParameters(const UNiagaraRendererProperties& Renderer)
    {
        if (const UNiagaraSpriteRendererProperties* Sprite = Cast<UNiagaraSpriteRendererProperties>(&Renderer))
        {
            return &Sprite->MaterialParameters;
        }
        if (const UNiagaraMeshRendererProperties* Mesh = Cast<UNiagaraMeshRendererProperties>(&Renderer))
        {
            return &Mesh->MaterialParameters;
        }
        return nullptr;
    }

    // SubImageSize lives on the sprite and mesh renderer classes separately (there is no shared
    // base declaring it), so the two are named here rather than reached through reflection.
    bool ReadDeclaredSubImageSize(const UNiagaraRendererProperties& Renderer, FIntPoint& OutSize)
    {
        if (const UNiagaraSpriteRendererProperties* Sprite = Cast<UNiagaraSpriteRendererProperties>(&Renderer))
        {
            OutSize = FIntPoint(FMath::RoundToInt32(Sprite->SubImageSize.X), FMath::RoundToInt32(Sprite->SubImageSize.Y));
            return true;
        }
        if (const UNiagaraMeshRendererProperties* Mesh = Cast<UNiagaraMeshRendererProperties>(&Renderer))
        {
            OutSize = FIntPoint(FMath::RoundToInt32(Mesh->SubImageSize.X), FMath::RoundToInt32(Mesh->SubImageSize.Y));
            return true;
        }
        return false;
    }

    struct FMaterialTextureScan
    {
        // Textures sampled through a node whose whole purpose is to declare the flipbook.
        TArray<UTexture2D*> SubUVTextures;
        // Parameter name of the sole SubUV node when it is a parameter, so an instance override
        // or a renderer binding on the same name can be resolved.
        FName SubUVParameterName = NAME_None;
        // Every texture the graph samples, whatever the node.
        TArray<UTexture2D*> SampledTextures;
    };

    void ScanExpressionsForTextures(
        TConstArrayView<TObjectPtr<UMaterialExpression>> Expressions,
        TSet<const UMaterialFunctionInterface*>& VisitedFunctions,
        int32 Depth,
        FMaterialTextureScan& Out)
    {
        for (const TObjectPtr<UMaterialExpression>& ExpressionPtr : Expressions)
        {
            UMaterialExpression* Expression = ExpressionPtr.Get();
            if (!Expression)
            {
                continue;
            }

            if (const UMaterialExpressionTextureBase* TextureNode = Cast<UMaterialExpressionTextureBase>(Expression))
            {
                if (UTexture2D* Texture2D = Cast<UTexture2D>(TextureNode->Texture.Get()))
                {
                    Out.SampledTextures.AddUnique(Texture2D);
                    if (Expression->IsA<UMaterialExpressionParticleSubUV>()
                        || Expression->IsA<UMaterialExpressionTextureSampleParameterSubUV>())
                    {
                        Out.SubUVTextures.AddUnique(Texture2D);
                        const FName ParameterName = Expression->GetParameterName();
                        if (!ParameterName.IsNone())
                        {
                            Out.SubUVParameterName = ParameterName;
                        }
                    }
                }
                continue;
            }

            if (const UMaterialExpressionMaterialFunctionCall* Call = Cast<UMaterialExpressionMaterialFunctionCall>(Expression))
            {
                const UMaterialFunctionInterface* Function = Call->MaterialFunction.Get();
                if (Function && Depth < SubUVMaxFunctionDepth && !VisitedFunctions.Contains(Function))
                {
                    VisitedFunctions.Add(Function);
                    ScanExpressionsForTextures(Function->GetExpressions(), VisitedFunctions, Depth + 1, Out);
                }
            }
        }
    }

    // The texture the renderer's declared grid will actually be sliced into, and how it was
    // arrived at. Returns false with OutReason populated when it cannot be established - never
    // a guess, because a wrong texture produces a confident finding about the wrong asset.
    bool ResolveRendererSubUVTexture(
        const UNiagaraRendererProperties& Renderer,
        UTexture2D*& OutTexture,
        PinWrightNiagara::ESubUVTextureSource& OutSource,
        FString& OutMaterialPath,
        FString& OutReason)
    {
        OutTexture = nullptr;
        OutSource = PinWrightNiagara::ESubUVTextureSource::Unresolved;

        const FNiagaraRendererMaterialParameters* MaterialParameters = GetRendererMaterialParameters(Renderer);

        // A renderer texture-parameter binding drives a per-instance MID, so it overrides
        // whatever the material asset samples - check it before reading the material at all.
        TArray<UTexture2D*> BoundTextures;
        if (MaterialParameters)
        {
            for (const FNiagaraRendererMaterialTextureParameter& Parameter : MaterialParameters->TextureParameters)
            {
                if (UTexture2D* Bound = Cast<UTexture2D>(Parameter.Texture.Get()))
                {
                    BoundTextures.AddUnique(Bound);
                }
            }
        }
        if (BoundTextures.Num() == 1)
        {
            OutTexture = BoundTextures[0];
            OutSource = PinWrightNiagara::ESubUVTextureSource::RendererTextureParameter;
            return true;
        }

        TArray<UMaterialInterface*> Materials;
        Renderer.GetUsedMaterials(nullptr, Materials);
        Materials.RemoveAll([](const UMaterialInterface* Material) { return Material == nullptr; });
        if (Materials.Num() != 1)
        {
            OutReason = FString::Printf(
                TEXT("the renderer resolves %d material(s) outside a running emitter instance, so which material's texture the grid slices cannot be established"),
                Materials.Num());
            return false;
        }

        UMaterialInterface* MaterialInterface = Materials[0];
        OutMaterialPath = MaterialInterface->GetPathName();

        UMaterial* BaseMaterial = MaterialInterface->GetMaterial();
        if (!BaseMaterial)
        {
            OutReason = FString::Printf(TEXT("material '%s' has no base material graph to read"), *OutMaterialPath);
            return false;
        }

        FMaterialTextureScan Scan;
        TSet<const UMaterialFunctionInterface*> VisitedFunctions;
        ScanExpressionsForTextures(BaseMaterial->GetExpressions(), VisitedFunctions, 0, Scan);

        if (Scan.SubUVTextures.Num() == 1)
        {
            OutTexture = Scan.SubUVTextures[0];
            OutSource = PinWrightNiagara::ESubUVTextureSource::MaterialSubUVExpression;
        }
        else if (Scan.SubUVTextures.Num() > 1)
        {
            OutReason = FString::Printf(
                TEXT("material '%s' samples %d different textures through SubUV nodes, so which one the grid slices is ambiguous"),
                *OutMaterialPath, Scan.SubUVTextures.Num());
            return false;
        }
        else if (Scan.SampledTextures.Num() == 1)
        {
            OutTexture = Scan.SampledTextures[0];
            OutSource = PinWrightNiagara::ESubUVTextureSource::MaterialSoleTextureSample;
        }
        else
        {
            OutReason = Scan.SampledTextures.Num() == 0
                ? FString::Printf(TEXT("material '%s' samples no 2D texture, so there is no atlas to measure"), *OutMaterialPath)
                : FString::Printf(
                    TEXT("material '%s' samples %d 2D textures and none of them through a SubUV node, so which one is the atlas is ambiguous"),
                    *OutMaterialPath, Scan.SampledTextures.Num());
            return false;
        }

        // The sole SubUV sampler is a parameter: a renderer binding on the same name, or an
        // instance override, replaces the texture the base graph points at.
        if (!Scan.SubUVParameterName.IsNone())
        {
            if (MaterialParameters)
            {
                for (const FNiagaraRendererMaterialTextureParameter& Parameter : MaterialParameters->TextureParameters)
                {
                    if (Parameter.MaterialParameterName == Scan.SubUVParameterName)
                    {
                        if (UTexture2D* Bound = Cast<UTexture2D>(Parameter.Texture.Get()))
                        {
                            OutTexture = Bound;
                            OutSource = PinWrightNiagara::ESubUVTextureSource::RendererTextureParameter;
                            return true;
                        }
                    }
                }
            }

            UTexture* Overridden = nullptr;
            if (MaterialInterface->GetTextureParameterValue(FHashedMaterialParameterInfo(Scan.SubUVParameterName), Overridden))
            {
                if (UTexture2D* Overridden2D = Cast<UTexture2D>(Overridden))
                {
                    OutTexture = Overridden2D;
                }
            }
        }

        return true;
    }

    // One energy value per texel column and per texel row of the mip, in two passes over it.
    // These are the 1-D profiles the gutter period is read from: an atlas's inter-cell lines show
    // as regularly spaced minima in both.
    bool BuildAxisEnergyProfiles(UTexture2D* Texture, int32 MipIndex,
        TArray<double>& OutColumns, TArray<double>& OutRows, FString& OutError)
    {
        OutColumns.Reset();
        OutRows.Reset();

        const int32 Width = Texture ? static_cast<int32>(Texture->Source.GetSizeX()) : 0;
        const int32 Height = Texture ? static_cast<int32>(Texture->Source.GetSizeY()) : 0;
        if (Width < 8 || Height < 8)
        {
            OutError = FString::Printf(TEXT("the %d x %d source mip is too small to read a cell period from"), Width, Height);
            return false;
        }

        const int32 ColumnSamples = FMath::Min(Width, SubUVMaxProfileSamples);
        const int32 RowSamples = FMath::Min(Height, SubUVMaxProfileSamples);

        TArray<TexturePixelStats::FRegionStats> Strips;
        if (!TexturePixelStats::MeasureTileGrid(Texture, MipIndex, ColumnSamples, 1, Strips, OutError))
        {
            return false;
        }
        OutColumns.Reserve(Strips.Num());
        for (const TexturePixelStats::FRegionStats& Strip : Strips)
        {
            OutColumns.Add(TileEnergy(Strip));
        }

        if (!TexturePixelStats::MeasureTileGrid(Texture, MipIndex, 1, RowSamples, Strips, OutError))
        {
            return false;
        }
        OutRows.Reserve(Strips.Num());
        for (const TexturePixelStats::FRegionStats& Strip : Strips)
        {
            OutRows.Add(TileEnergy(Strip));
        }

        return true;
    }
}

int32 PinWrightNiagara::DetectAxisDivisions(const TArray<double>& Profile, int32 MaxDivisions, double& OutConfidence)
{
    OutConfidence = 0.0;

    const int32 SampleCount = Profile.Num();
    if (SampleCount < 8 || MaxDivisions < 2)
    {
        return 0;
    }

    double Sum = 0.0;
    double Peak = 0.0;
    for (const double Value : Profile)
    {
        Sum += Value;
        Peak = FMath::Max(Peak, Value);
    }
    const double Mean = Sum / SampleCount;
    if (Peak <= SubUVProfileFloor || Mean <= SubUVProfileFloor)
    {
        // Nothing is drawn on this axis, so no period can be read from it.
        return 0;
    }

    const int32 Ceiling = FMath::Min3(MaxDivisions, SampleCount / 4, SubUVMaxDetectedDivisions);
    int32 BestDivisions = 0;
    double BestConfidence = 0.0;

    for (int32 Divisions = 2; Divisions <= Ceiling; ++Divisions)
    {
        double CutPeak = 0.0;
        for (int32 Cut = 1; Cut < Divisions; ++Cut)
        {
            const int32 Center = static_cast<int32>((static_cast<int64>(Cut) * SampleCount) / Divisions);
            // The two samples straddling the cut: a gutter is a line between cells, and the cut
            // can land on either side of it.
            for (int32 Index = Center - 1; Index <= Center; ++Index)
            {
                if (Index >= 0 && Index < SampleCount)
                {
                    CutPeak = FMath::Max(CutPeak, Profile[Index]);
                }
            }
        }
        if (CutPeak > SubUVGutterMaxMeanRatio * Mean)
        {
            continue;
        }

        // The populated slices must form a non-empty contiguous PREFIX, and there must be at
        // least two of them. A flipbook fills its cells in order, so trailing blanks are
        // ordinary - demanding content in every slice would reject the true grid of a sheet
        // holding fewer frames than it has cells, and then accept one of its divisors instead
        // (whose cuts are also real gutters), reporting a confidently wrong period. A GAP is
        // different: content after a blank slice means the dark cuts are just empty image, and
        // any division would pass on it.
        int32 PopulatedSlices = 0;
        bool bPopulatedPrefix = true;
        for (int32 Slice = 0; Slice < Divisions; ++Slice)
        {
            const int32 Start = static_cast<int32>((static_cast<int64>(Slice) * SampleCount) / Divisions);
            const int32 End = static_cast<int32>((static_cast<int64>(Slice + 1) * SampleCount) / Divisions);
            double SlicePeak = 0.0;
            for (int32 Index = Start; Index < End; ++Index)
            {
                SlicePeak = FMath::Max(SlicePeak, Profile[Index]);
            }
            if (SlicePeak >= SubUVSliceContentRatio * Peak)
            {
                if (PopulatedSlices != Slice)
                {
                    bPopulatedPrefix = false;
                    break;
                }
                ++PopulatedSlices;
            }
        }
        if (!bPopulatedPrefix || PopulatedSlices < 2)
        {
            continue;
        }

        // Largest passing division wins - see the header for why the smallest is wrong.
        BestDivisions = Divisions;
        BestConfidence = 1.0 - FMath::Min(1.0, CutPeak / Mean);
    }

    OutConfidence = BestConfidence;
    return BestDivisions;
}

FIntPoint PinWrightNiagara::DetectPopulatedGrid(const TArray<TexturePixelStats::FRegionStats>& Tiles,
    int32 Columns, int32 Rows, int32& OutEmptyTileCount)
{
    OutEmptyTileCount = 0;
    if (Columns < 1 || Rows < 1 || Tiles.Num() != Columns * Rows)
    {
        return FIntPoint::ZeroValue;
    }

    TArray<bool> Blank;
    Blank.SetNumUninitialized(Tiles.Num());
    for (int32 Index = 0; Index < Tiles.Num(); ++Index)
    {
        Blank[Index] = IsBlankSubUVTile(Tiles[Index]);
        if (Blank[Index])
        {
            ++OutEmptyTileCount;
        }
    }

    // A wholly blank texture says nothing about its layout, so it is unmeasured rather than a
    // zero-sized grid.
    if (OutEmptyTileCount == Tiles.Num())
    {
        return FIntPoint::ZeroValue;
    }

    int32 LastColumn = Columns - 1;
    for (; LastColumn >= 0; --LastColumn)
    {
        bool bColumnBlank = true;
        for (int32 Row = 0; Row < Rows && bColumnBlank; ++Row)
        {
            bColumnBlank = Blank[Row * Columns + LastColumn];
        }
        if (!bColumnBlank)
        {
            break;
        }
    }

    int32 LastRow = Rows - 1;
    for (; LastRow >= 0; --LastRow)
    {
        bool bRowBlank = true;
        for (int32 Column = 0; Column < Columns && bRowBlank; ++Column)
        {
            bRowBlank = Blank[LastRow * Columns + Column];
        }
        if (!bRowBlank)
        {
            break;
        }
    }

    return FIntPoint(LastColumn + 1, LastRow + 1);
}

bool PinWrightNiagara::ParseGridFromAssetName(const FString& AssetName, FIntPoint& OutGrid)
{
    OutGrid = FIntPoint::ZeroValue;

    bool bFound = false;
    for (int32 Index = 1; Index + 1 < AssetName.Len(); ++Index)
    {
        const TCHAR Separator = AssetName[Index];
        if (Separator != TEXT('x') && Separator != TEXT('X'))
        {
            continue;
        }

        int32 LeftStart = Index;
        while (LeftStart > 0 && FChar::IsDigit(AssetName[LeftStart - 1]))
        {
            --LeftStart;
        }
        int32 RightEnd = Index + 1;
        while (RightEnd < AssetName.Len() && FChar::IsDigit(AssetName[RightEnd]))
        {
            ++RightEnd;
        }

        const int32 LeftLen = Index - LeftStart;
        const int32 RightLen = RightEnd - (Index + 1);
        // 1-2 digits each: a longer run is a pixel size (1024x1024), not a frame grid.
        if (LeftLen < 1 || LeftLen > 2 || RightLen < 1 || RightLen > 2)
        {
            continue;
        }

        const int32 Columns = FCString::Atoi(*AssetName.Mid(LeftStart, LeftLen));
        const int32 RowsValue = FCString::Atoi(*AssetName.Mid(Index + 1, RightLen));
        if (Columns < 1 || Columns > 64 || RowsValue < 1 || RowsValue > 64)
        {
            continue;
        }

        // Keep scanning: the last run in the name wins, so a trailing "_8x8" beats an earlier one.
        OutGrid = FIntPoint(Columns, RowsValue);
        bFound = true;
    }

    return bFound;
}

bool PinWrightNiagara::EvaluateRendererSubUVAtlas(const UNiagaraRendererProperties& Renderer, FSubUVRendererAtlasFinding& OutFinding)
{
    OutFinding = FSubUVRendererAtlasFinding();
    OutFinding.RendererClass = Renderer.GetClass()->GetPathName();

    FIntPoint Declared(1, 1);
    if (!ReadDeclaredSubImageSize(Renderer, Declared) || Declared == FIntPoint(1, 1))
    {
        return false;
    }
    OutFinding.DeclaredSubImageSize = Declared;

    if (Declared.X < 1 || Declared.Y < 1)
    {
        OutFinding.UnverifiedReason = TEXT("SubImageSize has a zero or negative axis, which is not a usable frame grid");
        return true;
    }

    UTexture2D* Texture = nullptr;
    FString Reason;
    if (!ResolveRendererSubUVTexture(Renderer, Texture, OutFinding.TextureSource, OutFinding.MaterialPath, Reason) || !Texture)
    {
        OutFinding.UnverifiedReason = Reason.IsEmpty() ? TEXT("the sampled texture could not be resolved") : Reason;
        return true;
    }

    OutFinding.TexturePath = Texture->GetPathName();

    // The source mip is what the measurements read, so the divisibility heuristic is applied to
    // the same dimensions rather than to the platform-data size.
    const int32 Width = Texture->Source.IsValid() ? static_cast<int32>(Texture->Source.GetSizeX()) : 0;
    const int32 Height = Texture->Source.IsValid() ? static_cast<int32>(Texture->Source.GetSizeY()) : 0;
    OutFinding.TextureWidth = Width;
    OutFinding.TextureHeight = Height;
    if (Width > 0 && Height > 0)
    {
        OutFinding.bDimensionsIndivisible = (Width % Declared.X) != 0 || (Height % Declared.Y) != 0;
    }

    FIntPoint NameGrid = FIntPoint::ZeroValue;
    if (ParseGridFromAssetName(Texture->GetName(), NameGrid))
    {
        OutFinding.NameEncodedGrid = NameGrid;
    }

    // Gutter period first: it is the only signal that reads the atlas's real cell size, and it is
    // the one that catches a FULLY PACKED sheet at a different grid - the case every other signal
    // here is blind to, because such a sheet has no empty cells, divides evenly, and usually
    // spells nothing in its name.
    TArray<double> ColumnProfile;
    TArray<double> RowProfile;
    FString ProfileError;
    if (BuildAxisEnergyProfiles(Texture, 0, ColumnProfile, RowProfile, ProfileError))
    {
        const int32 MaxDivisions = FMath::Clamp(FMath::Max(Declared.X, Declared.Y) * 2, 16, SubUVMaxDetectedDivisions);
        const int32 DetectedColumns = DetectAxisDivisions(ColumnProfile, MaxDivisions, OutFinding.DetectedColumnConfidence);
        const int32 DetectedRows = DetectAxisDivisions(RowProfile, MaxDivisions, OutFinding.DetectedRowConfidence);
        if (DetectedColumns > 0 && DetectedRows > 0)
        {
            OutFinding.DetectedGrid = FIntPoint(DetectedColumns, DetectedRows);
        }
        else
        {
            OutFinding.DetectionReason = TEXT("no regular gutter period is readable on both axes (frames drawn edge to edge, or a sheet with no inter-cell lines)");
        }
    }
    else
    {
        OutFinding.DetectionReason = ProfileError;
    }

    TArray<TexturePixelStats::FRegionStats> Tiles;
    FString MeasureError;
    if (!TexturePixelStats::MeasureTileGrid(Texture, 0, Declared.X, Declared.Y, Tiles, MeasureError))
    {
        OutFinding.UnverifiedReason = FString::Printf(
            TEXT("the atlas pixels could not be measured (%s)"), *MeasureError);
        return true;
    }

    OutFinding.PopulatedGrid = DetectPopulatedGrid(Tiles, Declared.X, Declared.Y, OutFinding.EmptyTileCount);
    if (OutFinding.PopulatedGrid.X <= 0 && OutFinding.DetectedGrid.X <= 0)
    {
        // Neither measurement produced a verdict, so nothing here covers the declared grid.
        OutFinding.UnverifiedReason = TEXT("every cell of the declared grid is blank, so the texture's real layout cannot be read from it");
    }
    return true;
}

PinWrightNiagara::ESubUVAtlasVerdict PinWrightNiagara::EvaluateSubUVAtlases(
    const UNiagaraSystem& System, TArray<FSubUVRendererAtlasFinding>& OutFindings)
{
    OutFindings.Reset();

    for (const FNiagaraEmitterHandle& Handle : System.GetEmitterHandles())
    {
        // A disabled handle renders nothing; DISABLED_EMITTER already reports it, and adding a
        // second finding about the same dead emitter is noise.
        if (!Handle.GetIsEnabled())
        {
            continue;
        }
        const FVersionedNiagaraEmitterData* EmitterData = Handle.GetEmitterData();
        if (!EmitterData)
        {
            continue;
        }

        const TArray<UNiagaraRendererProperties*>& Renderers = EmitterData->GetRenderers();
        for (int32 Index = 0; Index < Renderers.Num(); ++Index)
        {
            const UNiagaraRendererProperties* Renderer = Renderers[Index];
            if (!Renderer || !Renderer->GetIsEnabled())
            {
                continue;
            }

            FSubUVRendererAtlasFinding Finding;
            if (!EvaluateRendererSubUVAtlas(*Renderer, Finding))
            {
                continue;
            }
            Finding.EmitterName = Handle.GetName().ToString();
            Finding.RendererIndex = Index;
            OutFindings.Add(MoveTemp(Finding));
        }
    }

    if (OutFindings.Num() == 0)
    {
        return ESubUVAtlasVerdict::NotApplicable;
    }

    bool bAnyMismatch = false;
    bool bAnyUnverified = false;
    for (const FSubUVRendererAtlasFinding& Finding : OutFindings)
    {
        bAnyMismatch |= Finding.IsMismatch();
        bAnyUnverified |= !Finding.UnverifiedReason.IsEmpty();
    }

    if (bAnyMismatch)
    {
        return ESubUVAtlasVerdict::Mismatched;
    }
    return bAnyUnverified ? ESubUVAtlasVerdict::Unverified : ESubUVAtlasVerdict::Consistent;
}

const TCHAR* PinWrightNiagara::SubUVAtlasVerdictToString(ESubUVAtlasVerdict Verdict)
{
    switch (Verdict)
    {
    case ESubUVAtlasVerdict::Consistent: return TEXT("consistent");
    case ESubUVAtlasVerdict::Mismatched: return TEXT("mismatched");
    case ESubUVAtlasVerdict::Unverified: return TEXT("unverified");
    default: return TEXT("not_applicable");
    }
}

FString PinWrightNiagara::DescribeSubUVAtlasFinding(const FSubUVRendererAtlasFinding& Finding)
{
    TArray<FString> Evidence;
    if (Finding.HasDetectedGridMismatch())
    {
        Evidence.Add(FString::Printf(
            TEXT("the atlas's own cell period measures %d x %d (gutter confidence %.2f / %.2f)"),
            Finding.DetectedGrid.X, Finding.DetectedGrid.Y,
            Finding.DetectedColumnConfidence, Finding.DetectedRowConfidence));
    }
    if (Finding.HasTrailingEmptyMismatch())
    {
        Evidence.Add(FString::Printf(
            TEXT("%d of the declared grid's %d cells carry no pixels at all, and dropping its wholly-empty trailing columns and rows leaves %d x %d ")
            TEXT("(weak on its own: a partly-filled sheet and an alpha-faded flipbook both leave a trailing row blank legitimately)"),
            Finding.EmptyTileCount,
            Finding.DeclaredSubImageSize.X * Finding.DeclaredSubImageSize.Y,
            Finding.PopulatedGrid.X,
            Finding.PopulatedGrid.Y));
    }
    if (Finding.HasNameMismatch())
    {
        Evidence.Add(FString::Printf(TEXT("the texture's name spells a %d x %d grid"),
            Finding.NameEncodedGrid.X, Finding.NameEncodedGrid.Y));
    }
    if (Finding.bDimensionsIndivisible)
    {
        Evidence.Add(FString::Printf(TEXT("%d x %d does not divide evenly by the declared grid, so no cell lines up with whole texels"),
            Finding.TextureWidth, Finding.TextureHeight));
    }

    return FString::Printf(
        TEXT("Renderer %d on emitter '%s' declares SubImageSize %d x %d over '%s' (%d x %d) - %s. ")
        TEXT("The vertex factory remaps every sprite's UVs into a 1/%d x 1/%d window unconditionally, so a wrong grid ")
        TEXT("samples across cell boundaries and renders clipped fragments and inter-cell gutter rather than a frame, ")
        TEXT("with no compile error and nothing else in this response to show it. ")
        TEXT("Set SubImageSize to the texture's real grid, or assign the atlas that matches it."),
        Finding.RendererIndex,
        *Finding.EmitterName,
        Finding.DeclaredSubImageSize.X,
        Finding.DeclaredSubImageSize.Y,
        *Finding.TexturePath,
        Finding.TextureWidth,
        Finding.TextureHeight,
        *FString::Join(Evidence, TEXT("; ")),
        Finding.DeclaredSubImageSize.X,
        Finding.DeclaredSubImageSize.Y);
}
