// Copyright (c) 2026 Alexander Penkin. MIT License.

// SplineHelpers.h - Shared spline point-type <-> string mapping.
//   ESplinePointType parse/stringify is needed by both the actor-scoped spline.* handlers
//   (SplineHandler.cpp) and the SCS-template blueprint.scs.set_spline_points handler
//   (SCSHandler.cpp). Consolidated here as inline named-namespace functions (the codebase's
//   Unity-safe convention, matching CollisionHelpers.h) so the string<->enum table lives in
//   one place instead of being copied per translation unit.
#pragma once
#include "CoreMinimal.h"
#include "Components/SplineComponent.h"

namespace SplineHelpers
{

// Parse a spline point-type string (case-insensitive) to the engine enum. Unknown -> Curve.
inline ESplinePointType::Type ParseSplinePointType(const FString& TypeStr)
{
    const FString LowerStr = TypeStr.ToLower();
    if (LowerStr == TEXT("linear")) return ESplinePointType::Linear;
    if (LowerStr == TEXT("curve")) return ESplinePointType::Curve;
    if (LowerStr == TEXT("constant")) return ESplinePointType::Constant;
    if (LowerStr == TEXT("curveclamped")) return ESplinePointType::CurveClamped;
    if (LowerStr == TEXT("curvecustomtangent")) return ESplinePointType::CurveCustomTangent;
    return ESplinePointType::Curve;
}

// Convert a spline point-type enum back to its canonical string.
inline FString SplinePointTypeToString(ESplinePointType::Type Type)
{
    switch (Type)
    {
        case ESplinePointType::Linear: return TEXT("Linear");
        case ESplinePointType::Curve: return TEXT("Curve");
        case ESplinePointType::Constant: return TEXT("Constant");
        case ESplinePointType::CurveClamped: return TEXT("CurveClamped");
        case ESplinePointType::CurveCustomTangent: return TEXT("CurveCustomTangent");
        default: return TEXT("Unknown");
    }
}

} // namespace SplineHelpers
