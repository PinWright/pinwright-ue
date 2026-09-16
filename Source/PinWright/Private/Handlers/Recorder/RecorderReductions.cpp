// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "RecorderReductions.h"
#include "RecorderAsOf.h"

#include <limits>

namespace RecorderModel
{

double FValuePoint::Magnitude() const
{
    switch (Kind)
    {
    case EValueKind::Float:
    case EValueKind::Int:
    case EValueKind::Bool:
    case EValueKind::Enum:
        return F0;
    case EValueKind::Vec2:
        return FMath::Sqrt(F0 * F0 + F1 * F1);
    case EValueKind::Vec3:
    case EValueKind::Rotator:
        return FMath::Sqrt(F0 * F0 + F1 * F1 + F2 * F2);
    case EValueKind::Vec4:
    case EValueKind::Quat:
        return FMath::Sqrt(F0 * F0 + F1 * F1 + F2 * F2 + F3 * F3);
    default:
        return std::numeric_limits<double>::quiet_NaN();
    }
}

bool FValuePoint::SignificantlyDiffers(const FValuePoint& Other, double Epsilon) const
{
    if (Kind != Other.Kind)
    {
        return true;
    }

    switch (Kind)
    {
    case EValueKind::String:
        return !S.Equals(Other.S, ESearchCase::CaseSensitive);

    case EValueKind::Enum:
    case EValueKind::Bool:
        // Enum identity is the underlying integer; bool is 0/1.
        return F0 != Other.F0;

    case EValueKind::Float:
    case EValueKind::Int:
        return FMath::Abs(F0 - Other.F0) > Epsilon;

    case EValueKind::Vec2:
    {
        const double Dx = F0 - Other.F0;
        const double Dy = F1 - Other.F1;
        return FMath::Sqrt(Dx * Dx + Dy * Dy) > Epsilon;
    }
    case EValueKind::Vec3:
    case EValueKind::Rotator:
    {
        const double Dx = F0 - Other.F0;
        const double Dy = F1 - Other.F1;
        const double Dz = F2 - Other.F2;
        return FMath::Sqrt(Dx * Dx + Dy * Dy + Dz * Dz) > Epsilon;
    }
    case EValueKind::Vec4:
    case EValueKind::Quat:
    {
        const double Dx = F0 - Other.F0;
        const double Dy = F1 - Other.F1;
        const double Dz = F2 - Other.F2;
        const double Dw = F3 - Other.F3;
        return FMath::Sqrt(Dx * Dx + Dy * Dy + Dz * Dz + Dw * Dw) > Epsilon;
    }
    default:
        return true;
    }
}

EValueKind KindFromString(const FString& In)
{
    if (In == TEXT("Float"))   return EValueKind::Float;
    if (In == TEXT("Int"))     return EValueKind::Int;
    if (In == TEXT("Bool"))    return EValueKind::Bool;
    if (In == TEXT("Vec2"))    return EValueKind::Vec2;
    if (In == TEXT("Vec3"))    return EValueKind::Vec3;
    if (In == TEXT("Vec4"))    return EValueKind::Vec4;
    if (In == TEXT("Quat"))    return EValueKind::Quat;
    if (In == TEXT("Rotator")) return EValueKind::Rotator;
    if (In == TEXT("Enum"))    return EValueKind::Enum;
    if (In == TEXT("String"))  return EValueKind::String;
    return EValueKind::Float;
}

const TCHAR* KindToString(EValueKind Kind)
{
    switch (Kind)
    {
    case EValueKind::Float:   return TEXT("Float");
    case EValueKind::Int:     return TEXT("Int");
    case EValueKind::Bool:    return TEXT("Bool");
    case EValueKind::Vec2:    return TEXT("Vec2");
    case EValueKind::Vec3:    return TEXT("Vec3");
    case EValueKind::Vec4:    return TEXT("Vec4");
    case EValueKind::Quat:    return TEXT("Quat");
    case EValueKind::Rotator: return TEXT("Rotator");
    case EValueKind::Enum:    return TEXT("Enum");
    case EValueKind::String:  return TEXT("String");
    default:                  return TEXT("Float");
    }
}

} // namespace RecorderModel

namespace RecorderReductions
{

FSummary Summarize(const TArray<FChangePoint>& Series, double From, double To)
{
    FSummary Out;

    FChangePoint StartPoint;
    if (RecorderAsOf::ValueAt(Series, From, StartPoint))
    {
        Out.bStartSet = true;
        Out.Start = StartPoint.Value.Magnitude();
    }
    FChangePoint EndPoint;
    if (RecorderAsOf::ValueAt(Series, To, EndPoint))
    {
        Out.bEndSet = true;
        Out.End = EndPoint.Value.Magnitude();
    }
    if (Out.bStartSet && Out.bEndSet)
    {
        Out.bNetDeltaSet = true;
        Out.NetDelta = Out.End - Out.Start;
    }

    int32 First = 0;
    int32 Last = -1;
    RecorderAsOf::SliceRange(Series, From, To, First, Last);

    if (First > Last)
    {
        // No change point inside the window: extrema collapse to the carried-forward
        // endpoints so a steady value still reports a sensible min/max/mean.
        Out.bMinSet = Out.bStartSet;
        Out.Min = Out.Start;
        Out.bMaxSet = Out.bEndSet;
        Out.Max = Out.End;
        Out.bMeanSet = Out.bStartSet;
        Out.Mean = Out.Start;
        return Out;
    }

    double Min = TNumericLimits<double>::Max();
    double Max = TNumericLimits<double>::Lowest();
    double Sum = 0.0;
    double ArgMinTs = Series[First].Ts;
    double ArgMaxTs = Series[First].Ts;
    int32 Count = 0;
    for (int32 i = First; i <= Last; ++i)
    {
        const double M = Series[i].Value.Magnitude();
        Sum += M;
        ++Count;
        if (M < Min) { Min = M; ArgMinTs = Series[i].Ts; }
        if (M > Max) { Max = M; ArgMaxTs = Series[i].Ts; }
    }

    Out.bMinSet = true; Out.Min = Min;
    Out.bMaxSet = true; Out.Max = Max;
    Out.bMeanSet = Count > 0; Out.Mean = Count > 0 ? Sum / Count : 0.0;
    Out.bArgMaxSet = true; Out.ArgMaxTs = ArgMaxTs;
    Out.bArgMinSet = true; Out.ArgMinTs = ArgMinTs;
    Out.ChangeCount = Count;
    return Out;
}

void Diff(const TArray<FChangePoint>& Series, double From, double To,
          int32 MaxPoints, int32 Cursor,
          TArray<FChangePoint>& OutPoints, int32& OutTotal, int32& OutElided, int32& OutNextCursor)
{
    OutPoints.Reset();

    int32 First = 0;
    int32 Last = -1;
    RecorderAsOf::SliceRange(Series, From, To, First, Last);
    OutTotal = First > Last ? 0 : (Last - First + 1);

    if (OutTotal == 0)
    {
        OutElided = 0;
        OutNextCursor = INDEX_NONE;
        return;
    }

    const int32 StartOffset = FMath::Max(0, Cursor);
    const int32 StartIndex = First + StartOffset;
    int32 Emitted = 0;
    for (int32 i = StartIndex; i <= Last && Emitted < MaxPoints; ++i, ++Emitted)
    {
        OutPoints.Add(Series[i]);
    }

    const int32 Consumed = StartOffset + Emitted;
    OutElided = OutTotal - Consumed;
    OutNextCursor = Consumed < OutTotal ? Consumed : INDEX_NONE;
}

void Downsample(const TArray<FChangePoint>& Series, double From, double To,
                int32 MaxPoints,
                TArray<FChangePoint>& OutPoints, int32& OutTotal, int32& OutElided)
{
    OutPoints.Reset();

    int32 First = 0;
    int32 Last = -1;
    RecorderAsOf::SliceRange(Series, From, To, First, Last);
    OutTotal = First > Last ? 0 : (Last - First + 1);

    if (OutTotal == 0)
    {
        OutElided = 0;
        return;
    }

    if (OutTotal <= MaxPoints || MaxPoints <= 0)
    {
        for (int32 i = First; i <= Last; ++i)
        {
            OutPoints.Add(Series[i]);
        }
        OutElided = 0;
        return;
    }

    // Always keep endpoints; spend the rest of the budget on per-bucket peaks.
    OutPoints.Add(Series[First]);
    const int32 InteriorBudget = FMath::Max(0, MaxPoints - 2);
    if (InteriorBudget > 0)
    {
        const int32 InteriorCount = OutTotal - 2;
        if (InteriorCount > 0)
        {
            const double BucketSize = (double)InteriorCount / InteriorBudget;
            const int32 InteriorStart = First + 1;
            for (int32 b = 0; b < InteriorBudget; ++b)
            {
                int32 Lo = InteriorStart + (int32)(b * BucketSize);
                int32 Hi = InteriorStart + (int32)((b + 1) * BucketSize) - 1;
                if (Hi < Lo) Hi = Lo;
                if (Lo > Last - 1) break;
                if (Hi > Last - 1) Hi = Last - 1;

                int32 PeakIdx = Lo;
                double PeakMag = Series[Lo].Value.Magnitude();
                for (int32 i = Lo + 1; i <= Hi; ++i)
                {
                    const double M = Series[i].Value.Magnitude();
                    if (M > PeakMag) { PeakMag = M; PeakIdx = i; }
                }
                OutPoints.Add(Series[PeakIdx]);
            }
        }
    }
    OutPoints.Add(Series[Last]);

    OutElided = OutTotal - OutPoints.Num();
}

} // namespace RecorderReductions
