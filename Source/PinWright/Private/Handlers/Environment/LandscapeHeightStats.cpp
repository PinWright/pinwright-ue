// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/Environment/LandscapeHeightStats.h"

#include "Dom/JsonValue.h"

namespace LandscapeHeightStats
{
    double HeightToWorldZ(double Height, double ScaleZ, double ActorLocationZ)
    {
        // Inverse of the sculpt/edit height write (LandscapeHandler.cpp): a Flatten targets
        // Height = (WorldZ - ActorLocationZ) / ScaleZ * 128 + 32768, so
        // WorldZ = (Height - 32768) / 128 * ScaleZ + ActorLocationZ.
        return (Height - 32768.0) / 128.0 * ScaleZ + ActorLocationZ;
    }

    FResolvedHeightRegion ResolveHeightRegion(
        const TOptional<int32>& ReqMinX, const TOptional<int32>& ReqMinY,
        const TOptional<int32>& ReqMaxX, const TOptional<int32>& ReqMaxY,
        int32 FullMinX, int32 FullMinY, int32 FullMaxX, int32 FullMaxY)
    {
        FResolvedHeightRegion R;
        // A set optional is honored verbatim (including a negative coordinate on a landscape whose
        // extent starts below zero); an unset optional defaults to the corresponding full extent.
        const int32 WantMinX = ReqMinX.IsSet() ? ReqMinX.GetValue() : FullMinX;
        const int32 WantMinY = ReqMinY.IsSet() ? ReqMinY.GetValue() : FullMinY;
        const int32 WantMaxX = ReqMaxX.IsSet() ? ReqMaxX.GetValue() : FullMaxX;
        const int32 WantMaxY = ReqMaxY.IsSet() ? ReqMaxY.GetValue() : FullMaxY;

        // Overlap is decided on the REQUESTED rectangle, before the clamp, because the clamp
        // destroys the evidence: a request lying wholly past an edge has all four coordinates
        // pulled onto that edge and comes back as a valid one-pixel region, which the caller then
        // reads as a real (and flat) measurement of terrain it never asked about.
        R.bOverlapsExtent = (WantMinX <= FullMaxX) && (WantMaxX >= FullMinX) &&
                            (WantMinY <= FullMaxY) && (WantMaxY >= FullMinY);

        R.MinX = FMath::Clamp(WantMinX, FullMinX, FullMaxX);
        R.MinY = FMath::Clamp(WantMinY, FullMinY, FullMaxY);
        R.MaxX = FMath::Clamp(WantMaxX, FullMinX, FullMaxX);
        R.MaxY = FMath::Clamp(WantMaxY, FullMinY, FullMaxY);

        // Only a coordinate the caller supplied can be "clamped"; an absent one defaulted to the
        // extent and was never a request that went unmet.
        R.bClamped = (ReqMinX.IsSet() && R.MinX != WantMinX) ||
                     (ReqMinY.IsSet() && R.MinY != WantMinY) ||
                     (ReqMaxX.IsSet() && R.MaxX != WantMaxX) ||
                     (ReqMaxY.IsSet() && R.MaxY != WantMaxY);

        R.bValid = R.bOverlapsExtent && (R.MinX <= R.MaxX) && (R.MinY <= R.MaxY);
        return R;
    }

    FMeasuredHeightRegion ResolveMeasuredRegion(
        int32 AskedMinX, int32 AskedMinY, int32 AskedMaxX, int32 AskedMaxY,
        int32 ReturnedMinX, int32 ReturnedMinY, int32 ReturnedMaxX, int32 ReturnedMaxY)
    {
        FMeasuredHeightRegion M;
        if (AskedMinX > AskedMaxX || AskedMinY > AskedMaxY)
        {
            return M;
        }
        const int64 AskedCount = ((int64)AskedMaxX - (int64)AskedMinX + 1) *
                                 ((int64)AskedMaxY - (int64)AskedMinY + 1);
        // Default position: nothing was measured, everything asked for is fill. The intersection
        // below is what earns any of it back.
        M.FabricatedSampleCount = AskedCount;

        // int64 throughout so the engine's INT_MAX / INT_MIN seeds reach the emptiness test
        // without overflowing on the way. No sentinel is named here: an unnarrowed seed simply
        // produces an empty intersection, which is the same answer as "found no components".
        const int64 MinX = FMath::Max((int64)AskedMinX, (int64)ReturnedMinX);
        const int64 MinY = FMath::Max((int64)AskedMinY, (int64)ReturnedMinY);
        const int64 MaxX = FMath::Min((int64)AskedMaxX, (int64)ReturnedMaxX);
        const int64 MaxY = FMath::Min((int64)AskedMaxY, (int64)ReturnedMaxY);
        if (MinX > MaxX || MinY > MaxY)
        {
            return M;
        }

        // The intersection lies inside the asked-for rectangle, so these fit int32 by construction.
        M.bAnyMeasured = true;
        M.MinX = (int32)MinX;
        M.MinY = (int32)MinY;
        M.MaxX = (int32)MaxX;
        M.MaxY = (int32)MaxY;
        M.MeasuredSampleCount = (MaxX - MinX + 1) * (MaxY - MinY + 1);
        M.FabricatedSampleCount = AskedCount - M.MeasuredSampleCount;
        M.bFullyMeasured = (M.FabricatedSampleCount == 0);
        return M;
    }

    bool ExtractSubRegion(
        const TArray<uint16>& In, int32 BufMinX, int32 BufMinY, int32 BufSizeX, int32 BufSizeY,
        int32 SubMinX, int32 SubMinY, int32 SubMaxX, int32 SubMaxY, TArray<uint16>& Out)
    {
        Out.Reset();
        if (BufSizeX <= 0 || BufSizeY <= 0 || (int64)In.Num() != (int64)BufSizeX * (int64)BufSizeY)
        {
            return false;
        }
        if (SubMinX > SubMaxX || SubMinY > SubMaxY ||
            (int64)SubMinX < (int64)BufMinX || (int64)SubMinY < (int64)BufMinY ||
            (int64)SubMaxX > (int64)BufMinX + BufSizeX - 1 ||
            (int64)SubMaxY > (int64)BufMinY + BufSizeY - 1)
        {
            return false;
        }

        const int32 SubSizeX = SubMaxX - SubMinX + 1;
        const int32 SubSizeY = SubMaxY - SubMinY + 1;
        Out.Reserve(SubSizeX * SubSizeY);
        for (int32 Row = 0; Row < SubSizeY; ++Row)
        {
            const int32 RowStart = (SubMinY - BufMinY + Row) * BufSizeX + (SubMinX - BufMinX);
            Out.Append(In.GetData() + RowStart, SubSizeX);
        }
        return true;
    }

    TSharedPtr<FJsonObject> BuildHeightStatsJson(
        const TArray<uint16>& Heights, int32 SizeX, int32 SizeY,
        double ScaleZ, double ActorLocationZ,
        bool bIncludeSamples, int32 MaxSamples, FString& OutError)
    {
        const int64 Expected = (int64)SizeX * (int64)SizeY;
        if (SizeX <= 0 || SizeY <= 0 || Expected <= 0)
        {
            OutError = TEXT("Empty height region");
            return nullptr;
        }
        if ((int64)Heights.Num() != Expected)
        {
            OutError = FString::Printf(
                TEXT("Height buffer size %d does not match region %dx%d"),
                Heights.Num(), SizeX, SizeY);
            return nullptr;
        }

        uint16 MinHeight = MAX_uint16;
        uint16 MaxHeight = 0;
        double Sum = 0.0;
        for (uint16 H : Heights)
        {
            MinHeight = FMath::Min(MinHeight, H);
            MaxHeight = FMath::Max(MaxHeight, H);
            Sum += (double)H;
        }
        const double MeanHeight = Sum / (double)Heights.Num();

        TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
        Out->SetNumberField(TEXT("sizeX"), SizeX);
        Out->SetNumberField(TEXT("sizeY"), SizeY);
        Out->SetNumberField(TEXT("sampleCount"), Heights.Num());
        Out->SetNumberField(TEXT("minHeight"), MinHeight);
        Out->SetNumberField(TEXT("maxHeight"), MaxHeight);
        Out->SetNumberField(TEXT("meanHeight"), MeanHeight);
        // The affine H->WorldZ transform is linear, so the world Z of the mean height equals
        // the mean of the per-sample world Z; compute it directly from MeanHeight via the same
        // HeightToWorldZ used for min/max (now double-typed, so the fractional mean flows through
        // the one formula rather than re-inlining the 32768/128 convention).
        Out->SetNumberField(TEXT("minZ"), HeightToWorldZ(MinHeight, ScaleZ, ActorLocationZ));
        Out->SetNumberField(TEXT("maxZ"), HeightToWorldZ(MaxHeight, ScaleZ, ActorLocationZ));
        Out->SetNumberField(TEXT("meanZ"), HeightToWorldZ(MeanHeight, ScaleZ, ActorLocationZ));

        if (bIncludeSamples)
        {
            const int32 Cap = (MaxSamples > 0) ? FMath::Min(MaxSamples, Heights.Num()) : Heights.Num();
            TArray<TSharedPtr<FJsonValue>> Arr;
            Arr.Reserve(Cap);
            for (int32 i = 0; i < Cap; ++i)
            {
                Arr.Add(MakeShared<FJsonValueNumber>((double)Heights[i]));
            }
            Out->SetArrayField(TEXT("heights"), Arr);
            Out->SetBoolField(TEXT("samplesTruncated"), Cap < Heights.Num());
        }

        OutError.Empty();
        return Out;
    }
}
