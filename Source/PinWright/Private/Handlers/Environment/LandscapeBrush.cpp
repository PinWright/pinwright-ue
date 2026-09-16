// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/Environment/LandscapeBrush.h"

namespace PinWright::LandscapeBrush
{
    bool ParseToolMode(const FString& In, EToolMode& Out)
    {
        if (In.Equals(TEXT("Raise"), ESearchCase::IgnoreCase))   { Out = EToolMode::Raise;   return true; }
        if (In.Equals(TEXT("Lower"), ESearchCase::IgnoreCase))   { Out = EToolMode::Lower;   return true; }
        if (In.Equals(TEXT("Flatten"), ESearchCase::IgnoreCase)) { Out = EToolMode::Flatten; return true; }
        if (In.Equals(TEXT("Smooth"), ESearchCase::IgnoreCase))  { Out = EToolMode::Smooth;  return true; }
        return false;
    }

    bool ParseFalloffProfile(const FString& In, EFalloffProfile& Out)
    {
        if (In.Equals(TEXT("Linear"), ESearchCase::IgnoreCase))    { Out = EFalloffProfile::Linear;    return true; }
        if (In.Equals(TEXT("Smooth"), ESearchCase::IgnoreCase))    { Out = EFalloffProfile::Smooth;    return true; }
        // Accept the mathematical spelling as well: callers reach for "smoothstep" far more
        // readily than for the engine's own control name, and silently treating it as an
        // unknown profile would be an error over a synonym.
        if (In.Equals(TEXT("Smoothstep"), ESearchCase::IgnoreCase)) { Out = EFalloffProfile::Smooth;   return true; }
        if (In.Equals(TEXT("Spherical"), ESearchCase::IgnoreCase)) { Out = EFalloffProfile::Spherical; return true; }
        if (In.Equals(TEXT("Tip"), ESearchCase::IgnoreCase))       { Out = EFalloffProfile::Tip;       return true; }
        return false;
    }

    const TCHAR* ToolModeName(EToolMode Mode)
    {
        switch (Mode)
        {
        case EToolMode::Raise:   return TEXT("Raise");
        case EToolMode::Lower:   return TEXT("Lower");
        case EToolMode::Flatten: return TEXT("Flatten");
        case EToolMode::Smooth:  return TEXT("Smooth");
        }
        return TEXT("Raise");
    }

    const TCHAR* FalloffProfileName(EFalloffProfile Profile)
    {
        switch (Profile)
        {
        case EFalloffProfile::Linear:    return TEXT("linear");
        case EFalloffProfile::Smooth:    return TEXT("smooth");
        case EFalloffProfile::Spherical: return TEXT("spherical");
        case EFalloffProfile::Tip:       return TEXT("tip");
        }
        return TEXT("linear");
    }

    FString ValidToolModes()
    {
        return TEXT("Raise, Lower, Flatten, Smooth");
    }

    FString ValidFalloffProfiles()
    {
        return TEXT("linear, smooth (alias smoothstep), spherical, tip");
    }

    float EvaluateFalloff(EFalloffProfile Profile, double DistanceUu, double RadiusUu, double FalloffFraction)
    {
        if (!(RadiusUu > 0.0) || DistanceUu > RadiusUu)
        {
            // Outside the brush, or a degenerate radius. Zero is the correct weight and
            // also the safe default (rpc-design.md §2: the zero value is a no-op, never
            // an accidental full-strength edit).
            return 0.0f;
        }

        const double Falloff = FMath::Clamp(FalloffFraction, 0.0, 1.0);
        const double FalloffWidthUu = RadiusUu * Falloff;
        if (!(FalloffWidthUu > 0.0))
        {
            // Hard edge: full weight everywhere inside, nothing outside. Matches the
            // previous handler, whose ramp branch could not be reached with falloff 0.
            return 1.0f;
        }

        // t runs 0 at the outer rim to 1 at the inner plateau edge, and saturates at 1
        // across the plateau itself.
        const double T = FMath::Clamp((RadiusUu - DistanceUu) / FalloffWidthUu, 0.0, 1.0);

        double Alpha = T;
        switch (Profile)
        {
        case EFalloffProfile::Linear:
            Alpha = T;
            break;
        case EFalloffProfile::Smooth:
            Alpha = T * T * (3.0 - 2.0 * T);
            break;
        case EFalloffProfile::Spherical:
            Alpha = FMath::Sqrt(FMath::Max(0.0, 1.0 - FMath::Square(1.0 - T)));
            break;
        case EFalloffProfile::Tip:
            Alpha = 1.0 - FMath::Sqrt(FMath::Max(0.0, 1.0 - FMath::Square(T)));
            break;
        }

        return (float)FMath::Clamp(Alpha, 0.0, 1.0);
    }

    void ClosestPointOnSegment(
        double Px, double Py,
        const FStrokeVertex& A, const FStrokeVertex& B,
        double ScaleX, double ScaleY,
        double& OutDistanceUu, double& OutWorldZ)
    {
        // Solve in world centimetres rather than local vertex units. On a landscape with
        // ScaleX != ScaleY the two spaces disagree about WHERE along the segment the
        // closest point lies, and it is the world answer that decides both the brush
        // weight and the interpolated flatten height.
        const double Sx = (FMath::Abs(ScaleX) > UE_DOUBLE_SMALL_NUMBER) ? FMath::Abs(ScaleX) : 1.0;
        const double Sy = (FMath::Abs(ScaleY) > UE_DOUBLE_SMALL_NUMBER) ? FMath::Abs(ScaleY) : 1.0;

        const double Ax = A.X * Sx, Ay = A.Y * Sy;
        const double Bx = B.X * Sx, By = B.Y * Sy;
        const double Qx = Px * Sx,  Qy = Py * Sy;

        const double Dx = Bx - Ax;
        const double Dy = By - Ay;
        const double LenSq = Dx * Dx + Dy * Dy;

        // A zero-length segment is the single-stamp case and is fully supported: the
        // parameter collapses to 0 and the distance is a plain radial one, so
        // `location` and `path` share this code path exactly (see the header).
        double T = 0.0;
        if (LenSq > UE_DOUBLE_SMALL_NUMBER)
        {
            T = FMath::Clamp(((Qx - Ax) * Dx + (Qy - Ay) * Dy) / LenSq, 0.0, 1.0);
        }

        const double Cx = Ax + T * Dx;
        const double Cy = Ay + T * Dy;

        OutDistanceUu = FMath::Sqrt(FMath::Square(Qx - Cx) + FMath::Square(Qy - Cy));
        OutWorldZ = A.WorldZ + T * (B.WorldZ - A.WorldZ);
    }

    void SegmentBounds(
        const FStrokeVertex& A, const FStrokeVertex& B,
        double RadiusUu, double ScaleX, double ScaleY,
        double& OutMinX, double& OutMinY, double& OutMaxX, double& OutMaxY)
    {
        const double Sx = (FMath::Abs(ScaleX) > UE_DOUBLE_SMALL_NUMBER) ? FMath::Abs(ScaleX) : 1.0;
        const double Sy = (FMath::Abs(ScaleY) > UE_DOUBLE_SMALL_NUMBER) ? FMath::Abs(ScaleY) : 1.0;

        // The radius is a world distance; converting it back into local vertex units is
        // per-axis, which is the whole point - one shared `Radius / ScaleX` is what made
        // the old brush an ellipse on a non-uniformly scaled landscape.
        const double PadX = RadiusUu / Sx;
        const double PadY = RadiusUu / Sy;

        OutMinX = FMath::Min(A.X, B.X) - PadX;
        OutMaxX = FMath::Max(A.X, B.X) + PadX;
        OutMinY = FMath::Min(A.Y, B.Y) - PadY;
        OutMaxY = FMath::Max(A.Y, B.Y) + PadY;
    }
}
