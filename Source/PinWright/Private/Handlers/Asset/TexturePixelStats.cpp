// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/Asset/TexturePixelStats.h"

#include "Engine/Texture.h"
#include "Engine/TextureDefines.h"
#include "Handlers/Asset/TextureSourceMipLock.h"
#include "Misc/Crc.h"
#include "UObject/Class.h"
#include "Utils/JsonBuilders.h"

namespace
{
    // A columns x rows grid is capped so one call cannot spill a response with tens of
    // thousands of stats blocks. 64x64 covers every atlas layout an author writes by hand.
    constexpr int32 MaxTileCount = 4096;

    // Builds an {r,g,b,a} child holding one numeric value per RGBA channel.
    void SetRgbaChannelObject(const TSharedPtr<FJsonObject>& Root, const TCHAR* Key,
        double R, double G, double B, double A)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetNumberField(TEXT("r"), R);
        Obj->SetNumberField(TEXT("g"), G);
        Obj->SetNumberField(TEXT("b"), B);
        Obj->SetNumberField(TEXT("a"), A);
        Root->SetObjectField(Key, Obj);
    }

    // Builds a {gray} child for single-channel (G8) sources.
    void SetGrayChannelObject(const TSharedPtr<FJsonObject>& Root, const TCHAR* Key, double Value)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetNumberField(TEXT("gray"), Value);
        Root->SetObjectField(Key, Obj);
    }

    // A read-only view of the mip a TextureSourceMip::FScopedMipLock is holding. Non-owning on
    // purpose: the lock itself is the single place in this plugin a source mip is taken and
    // released (it exists because two Critical lock-leak tickets came out of hand-rolled ones),
    // and it also flushes an in-flight async texture build first - without that, a read landing
    // during a DDC build reports "no pixel stats" for a texture that has them.
    struct FMipView
    {
        const uint8* Data = nullptr;
        int32 Width = 0;
        int32 Height = 0;
        ETextureSourceFormat Format = TSF_Invalid;

        int32 BytesPerPixel() const { return Format == TSF_BGRA8 ? 4 : 1; }
    };

    bool OpenMipView(const TextureSourceMip::FScopedMipLock& Lock, FMipView& Out, FString& OutError)
    {
        if (!Lock.IsValid())
        {
            // Already names the asset, the mip and the reason, and says whether retrying helps.
            OutError = Lock.GetError();
            return false;
        }

        Out.Data = Lock.GetReadData();
        if (!Out.Data)
        {
            OutError = TEXT("Source mip locked but returned no readable bytes");
            return false;
        }

        Out.Width = Lock.GetSizeX();
        Out.Height = Lock.GetSizeY();
        Out.Format = Lock.GetFormat();
        return true;
    }

    // Clamps a requested rectangle to the mip and refuses the ones that cannot be clamped
    // into something readable. An origin past the edge is an error rather than a clamp to a
    // zero-pixel rect: "0 pixels, mean 0" is how a wrong x/y passes for a measured answer.
    bool ResolveRegion(const TexturePixelStats::FPixelRegion& Requested, int32 MipWidth, int32 MipHeight,
        TexturePixelStats::FPixelRegion& Out, FString& OutError)
    {
        if (Requested.Width <= 0 || Requested.Height <= 0)
        {
            OutError = FString::Printf(
                TEXT("region width/height must be at least 1 pixel (got %d x %d)"),
                Requested.Width, Requested.Height);
            return false;
        }
        if (Requested.X < 0 || Requested.Y < 0)
        {
            OutError = FString::Printf(TEXT("region x/y must not be negative (got %d, %d)"), Requested.X, Requested.Y);
            return false;
        }
        if (Requested.X >= MipWidth || Requested.Y >= MipHeight)
        {
            OutError = FString::Printf(
                TEXT("region origin (%d, %d) is outside the %dx%d mip"),
                Requested.X, Requested.Y, MipWidth, MipHeight);
            return false;
        }

        Out.X = Requested.X;
        Out.Y = Requested.Y;
        Out.Width = FMath::Min(Requested.Width, MipWidth - Requested.X);
        Out.Height = FMath::Min(Requested.Height, MipHeight - Requested.Y);
        return true;
    }

    // Splits Area into Columns x Rows tiles, row-major. Boundaries are computed from the
    // running fraction rather than a fixed tile size, so an area that does not divide evenly
    // still covers every pixel exactly once instead of dropping the remainder.
    bool ComputeTiles(const TexturePixelStats::FPixelRegion& Area, int32 Columns, int32 Rows,
        TArray<TexturePixelStats::FPixelRegion>& Out, FString& OutError)
    {
        if (Columns < 1 || Rows < 1)
        {
            OutError = FString::Printf(
                TEXT("tileGrid columns and rows must both be at least 1 (got %d x %d)"), Columns, Rows);
            return false;
        }
        if (Columns > Area.Width || Rows > Area.Height)
        {
            OutError = FString::Printf(
                TEXT("tileGrid %d x %d does not fit the %d x %d pixel area being measured (a tile would be empty)"),
                Columns, Rows, Area.Width, Area.Height);
            return false;
        }
        if (static_cast<int64>(Columns) * static_cast<int64>(Rows) > static_cast<int64>(MaxTileCount))
        {
            OutError = FString::Printf(
                TEXT("tileGrid %d x %d exceeds the %d-tile cap"), Columns, Rows, MaxTileCount);
            return false;
        }

        Out.Reset();
        Out.Reserve(Columns * Rows);
        for (int32 Row = 0; Row < Rows; ++Row)
        {
            const int32 Y0 = Area.Y + static_cast<int32>((static_cast<int64>(Row) * Area.Height) / Rows);
            const int32 Y1 = Area.Y + static_cast<int32>((static_cast<int64>(Row + 1) * Area.Height) / Rows);
            for (int32 Column = 0; Column < Columns; ++Column)
            {
                const int32 X0 = Area.X + static_cast<int32>((static_cast<int64>(Column) * Area.Width) / Columns);
                const int32 X1 = Area.X + static_cast<int32>((static_cast<int64>(Column + 1) * Area.Width) / Columns);
                Out.Add(TexturePixelStats::FPixelRegion{ X0, Y0, X1 - X0, Y1 - Y0 });
            }
        }
        return true;
    }

    TexturePixelStats::FRegionStats AccumulateRegion(const FMipView& Mip, const TexturePixelStats::FPixelRegion& Rect)
    {
        TexturePixelStats::FRegionStats Stats;
        Stats.Region = Rect;
        Stats.PixelCount = static_cast<int64>(Rect.Width) * static_cast<int64>(Rect.Height);
        Stats.bSingleChannel = Mip.Format != TSF_BGRA8;

        // Rect is always at least 1x1 (ResolveRegion / ComputeTiles refuse anything smaller),
        // so this divides directly.
        const double Denom = static_cast<double>(Stats.PixelCount);
        const int32 BytesPerPixel = Mip.BytesPerPixel();

        if (!Stats.bSingleChannel)
        {
            // Source byte order is B,G,R,A (matches the desaturate/invert verbs).
            uint64 SumR = 0, SumG = 0, SumB = 0, SumA = 0;
            uint8 MinR = 255, MinG = 255, MinB = 255, MinA = 255;
            uint8 MaxR = 0, MaxG = 0, MaxB = 0, MaxA = 0;
            int32 MaxChannelSpread = 0;
            for (int32 Y = 0; Y < Rect.Height; ++Y)
            {
                const uint8* Row = Mip.Data
                    + (static_cast<int64>(Rect.Y + Y) * Mip.Width + Rect.X) * BytesPerPixel;
                for (int32 X = 0; X < Rect.Width; ++X)
                {
                    const int32 Idx = X * 4;
                    const uint8 B = Row[Idx + 0];
                    const uint8 G = Row[Idx + 1];
                    const uint8 R = Row[Idx + 2];
                    const uint8 A = Row[Idx + 3];
                    SumR += R; SumG += G; SumB += B; SumA += A;
                    MinR = FMath::Min(MinR, R); MaxR = FMath::Max(MaxR, R);
                    MinG = FMath::Min(MinG, G); MaxG = FMath::Max(MaxG, G);
                    MinB = FMath::Min(MinB, B); MaxB = FMath::Max(MaxB, B);
                    MinA = FMath::Min(MinA, A); MaxA = FMath::Max(MaxA, A);
                    const int32 Spread = FMath::Max3(
                        FMath::Abs(static_cast<int32>(R) - static_cast<int32>(G)),
                        FMath::Abs(static_cast<int32>(G) - static_cast<int32>(B)),
                        FMath::Abs(static_cast<int32>(R) - static_cast<int32>(B)));
                    MaxChannelSpread = FMath::Max(MaxChannelSpread, Spread);
                }
            }

            Stats.MeanR = SumR / Denom;
            Stats.MeanG = SumG / Denom;
            Stats.MeanB = SumB / Denom;
            Stats.MeanA = SumA / Denom;
            Stats.MinR = MinR; Stats.MinG = MinG; Stats.MinB = MinB; Stats.MinA = MinA;
            Stats.MaxR = MaxR; Stats.MaxG = MaxG; Stats.MaxB = MaxB; Stats.MaxA = MaxA;
            Stats.MaxChannelSpread = MaxChannelSpread;
            // grayscale <=> R==G==B for every pixel (a desaturate's success criterion).
            Stats.bGrayscale = MaxChannelSpread == 0;
            return Stats;
        }

        uint64 SumV = 0;
        uint8 MinV = 255, MaxV = 0;
        for (int32 Y = 0; Y < Rect.Height; ++Y)
        {
            const uint8* Row = Mip.Data
                + (static_cast<int64>(Rect.Y + Y) * Mip.Width + Rect.X) * BytesPerPixel;
            for (int32 X = 0; X < Rect.Width; ++X)
            {
                const uint8 V = Row[X];
                SumV += V;
                MinV = FMath::Min(MinV, V);
                MaxV = FMath::Max(MaxV, V);
            }
        }

        const double MeanV = SumV / Denom;
        Stats.MeanR = Stats.MeanG = Stats.MeanB = MeanV;
        Stats.MeanA = 255.0;
        Stats.MinR = Stats.MinG = Stats.MinB = MinV;
        Stats.MinA = 255;
        Stats.MaxR = Stats.MaxG = Stats.MaxB = MaxV;
        Stats.MaxA = 255;
        Stats.MaxChannelSpread = 0;
        // A single-channel source is grayscale by construction.
        Stats.bGrayscale = true;
        return Stats;
    }

    // channels / mean / min / max / maxChannelSpread / grayscale, in that order. Shared by the
    // top-level block and every tile so a tile is read with the same field vocabulary.
    void WriteStatsFields(const TSharedPtr<FJsonObject>& Obj, const TexturePixelStats::FRegionStats& Stats)
    {
        if (!Stats.bSingleChannel)
        {
            TArray<TSharedPtr<FJsonValue>> ChannelArray = {
                MakeShared<FJsonValueString>(TEXT("r")), MakeShared<FJsonValueString>(TEXT("g")),
                MakeShared<FJsonValueString>(TEXT("b")), MakeShared<FJsonValueString>(TEXT("a")) };
            Obj->SetArrayField(TEXT("channels"), ChannelArray);
            SetRgbaChannelObject(Obj, TEXT("mean"), Stats.MeanR, Stats.MeanG, Stats.MeanB, Stats.MeanA);
            SetRgbaChannelObject(Obj, TEXT("min"), Stats.MinR, Stats.MinG, Stats.MinB, Stats.MinA);
            SetRgbaChannelObject(Obj, TEXT("max"), Stats.MaxR, Stats.MaxG, Stats.MaxB, Stats.MaxA);
        }
        else
        {
            TArray<TSharedPtr<FJsonValue>> ChannelArray = { MakeShared<FJsonValueString>(TEXT("gray")) };
            Obj->SetArrayField(TEXT("channels"), ChannelArray);
            SetGrayChannelObject(Obj, TEXT("mean"), Stats.MeanR);
            SetGrayChannelObject(Obj, TEXT("min"), Stats.MinR);
            SetGrayChannelObject(Obj, TEXT("max"), Stats.MaxR);
        }
        Obj->SetNumberField(TEXT("maxChannelSpread"), Stats.MaxChannelSpread);
        Obj->SetBoolField(TEXT("grayscale"), Stats.bGrayscale);
    }

    TSharedPtr<FJsonObject> MakeRegionJson(const TexturePixelStats::FPixelRegion& Region)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetNumberField(TEXT("x"), Region.X);
        Obj->SetNumberField(TEXT("y"), Region.Y);
        Obj->SetNumberField(TEXT("width"), Region.Width);
        Obj->SetNumberField(TEXT("height"), Region.Height);
        return Obj;
    }
}

bool TexturePixelStats::IsReadableSourceFormat(ETextureSourceFormat Format)
{
    return Format == TSF_BGRA8 || Format == TSF_G8;
}

TSharedPtr<FJsonObject> TexturePixelStats::BuildPixelStatsJson(UTexture* Texture, int32 MipIndex, FString& OutError)
{
    FPixelStatsOptions Options;
    Options.MipIndex = MipIndex;
    return BuildPixelStatsJson(Texture, Options, OutError);
}

TSharedPtr<FJsonObject> TexturePixelStats::BuildPixelStatsJson(UTexture* Texture, const FPixelStatsOptions& Options, FString& OutError)
{
    OutError.Empty();

    TextureSourceMip::FScopedMipLock Lock(
        Texture, TEXT("source"), /*bReadOnly=*/true,
        TextureSourceMip::EFormatPolicy::Bgra8OrG8, Options.MipIndex);

    FMipView Mip;
    if (!OpenMipView(Lock, Mip, OutError))
    {
        return nullptr;
    }

    TexturePixelStats::FPixelRegion Area{ 0, 0, Mip.Width, Mip.Height };
    if (Options.bHasRegion && !ResolveRegion(Options.Region, Mip.Width, Mip.Height, Area, OutError))
    {
        return nullptr;
    }

    // Tiling is planned before anything is written so a rejected grid does not produce a
    // half-built response the caller has to tell apart from a whole-image one.
    TArray<TexturePixelStats::FPixelRegion> TileRects;
    if (Options.bHasTileGrid && !ComputeTiles(Area, Options.TileColumns, Options.TileRows, TileRects, OutError))
    {
        return nullptr;
    }

    const TexturePixelStats::FRegionStats Stats = AccumulateRegion(Mip, Area);

    TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
    Root->SetNumberField(TEXT("mip"), Options.MipIndex);
    // The mip's own size, not the measured area's: a caller needs to know what its region was
    // clamped against. pixelCount is what the stats actually cover, which is the whole mip
    // when no region was asked for.
    Root->SetNumberField(TEXT("width"), Mip.Width);
    Root->SetNumberField(TEXT("height"), Mip.Height);
    Root->SetStringField(TEXT("sourceFormat"), JsonBuilders::EnumValueToString(Mip.Format));
    Root->SetNumberField(TEXT("pixelCount"), static_cast<double>(Stats.PixelCount));
    WriteStatsFields(Root, Stats);

    // Content hash of the raw source mip bytes: two reads can be compared to answer
    // "did the pixels change at all" (e.g. an invert against its pre-invert hash).
    // Intentionally a separate whole-buffer pass rather than folded into the stat loop:
    // it hashes the full CalcMipSize() buffer, which the per-pixel loop (NumPixels =
    // Width*Height, one slice) does not necessarily cover byte-for-byte. It covers the whole
    // mip whatever `region` asked for, so its meaning never depends on the request shape.
    const int64 MipSize = Lock.GetMipSizeBytes();
    if (MipSize > 0 && MipSize <= static_cast<int64>(MAX_int32))
    {
        const uint32 Hash = FCrc::MemCrc32(Mip.Data, static_cast<int32>(MipSize));
        Root->SetStringField(TEXT("hash"), FString::Printf(TEXT("%08x"), Hash));
    }

    // Everything below is appended only when asked for, so the default response stays
    // byte-identical to the whole-mip one that shipped before regions existed.
    if (Options.bHasRegion)
    {
        Root->SetObjectField(TEXT("region"), MakeRegionJson(Area));
    }

    if (Options.bHasTileGrid)
    {
        TSharedPtr<FJsonObject> GridJson = MakeShared<FJsonObject>();
        GridJson->SetNumberField(TEXT("columns"), Options.TileColumns);
        GridJson->SetNumberField(TEXT("rows"), Options.TileRows);
        GridJson->SetNumberField(TEXT("tileCount"), TileRects.Num());
        Root->SetObjectField(TEXT("tileGrid"), GridJson);

        TArray<TSharedPtr<FJsonValue>> TilesJson;
        TilesJson.Reserve(TileRects.Num());
        for (int32 Index = 0; Index < TileRects.Num(); ++Index)
        {
            const TexturePixelStats::FRegionStats TileStats = AccumulateRegion(Mip, TileRects[Index]);
            TSharedPtr<FJsonObject> TileJson = MakeShared<FJsonObject>();
            TileJson->SetNumberField(TEXT("column"), Index % Options.TileColumns);
            TileJson->SetNumberField(TEXT("row"), Index / Options.TileColumns);
            TileJson->SetNumberField(TEXT("x"), TileStats.Region.X);
            TileJson->SetNumberField(TEXT("y"), TileStats.Region.Y);
            TileJson->SetNumberField(TEXT("width"), TileStats.Region.Width);
            TileJson->SetNumberField(TEXT("height"), TileStats.Region.Height);
            TileJson->SetNumberField(TEXT("pixelCount"), static_cast<double>(TileStats.PixelCount));
            WriteStatsFields(TileJson, TileStats);
            TilesJson.Add(MakeShared<FJsonValueObject>(TileJson));
        }
        Root->SetArrayField(TEXT("tiles"), TilesJson);
    }

    return Root;
}

bool TexturePixelStats::MeasureTileGrid(UTexture* Texture, int32 MipIndex, int32 Columns, int32 Rows,
    TArray<TexturePixelStats::FRegionStats>& OutTiles, FString& OutError)
{
    OutTiles.Reset();
    OutError.Empty();

    TextureSourceMip::FScopedMipLock Lock(
        Texture, TEXT("source"), /*bReadOnly=*/true,
        TextureSourceMip::EFormatPolicy::Bgra8OrG8, MipIndex);

    FMipView Mip;
    if (!OpenMipView(Lock, Mip, OutError))
    {
        return false;
    }

    const TexturePixelStats::FPixelRegion Area{ 0, 0, Mip.Width, Mip.Height };
    TArray<TexturePixelStats::FPixelRegion> TileRects;
    if (!ComputeTiles(Area, Columns, Rows, TileRects, OutError))
    {
        return false;
    }

    OutTiles.Reserve(TileRects.Num());
    for (const TexturePixelStats::FPixelRegion& Rect : TileRects)
    {
        OutTiles.Add(AccumulateRegion(Mip, Rect));
    }
    return true;
}
