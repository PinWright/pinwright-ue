// Copyright (c) 2026 Alexander Penkin. MIT License.

// Shared anonymous-namespace-style helpers used by widget inspect/export handlers.
// Centralized here to prevent ODR collisions across unity-build chunks.
#pragma once

#include "CoreMinimal.h"
#include "UObject/UnrealType.h"

namespace WidgetInspectHelpers
{

/** Strip a leading 'U' class prefix (e.g. "UButton" -> "Button"). */
inline FString StripClassPrefix(const FString& ClassName)
{
    if (ClassName.Len() > 1 && ClassName[0] == TEXT('U') && FChar::IsUpper(ClassName[1]))
    {
        return ClassName.Mid(1);
    }

    return ClassName;
}

/** True if a property should be surfaced in describe/snapshot output. */
inline bool ShouldDescribeProperty(const FProperty* Property)
{
    if (!Property)
    {
        return false;
    }

    if (Property->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated | CPF_DuplicateTransient))
    {
        return false;
    }

    return Property->HasAnyPropertyFlags(CPF_Edit | CPF_BlueprintVisible);
}

/** Format a float for a Geom.* attribute (3 decimal places). */
inline FString GeoFloat(float Value)
{
    return FString::Printf(TEXT("%.3f"), Value);
}

/** Format a double for a Geom.* attribute (3 decimal places). */
inline FString GeoFloat(double Value)
{
    return FString::Printf(TEXT("%.3f"), Value);
}

/** True if the 2D vector is not nearly zero. */
inline bool HasVector(const FVector2D& Value)
{
    return !Value.IsNearlyZero();
}

} // namespace WidgetInspectHelpers
