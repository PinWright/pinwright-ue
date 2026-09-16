// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"

class UMaterial;
class UMaterialExpression;
class UMaterialFunction;


class FMGIRLayoutEngine
{
public:
    struct FOptions
    {
        // Origin defaults are non-zero so the first laid-out slot (depth 0, lane 0)
        // does not land on (0,0) — that coordinate is the "unpositioned" sentinel
        // checked by LayoutExpressions, and writing it back would leave the node
        // visually unmoved and re-eligible for the next layout pass.
        int32 OriginX = 320;
        int32 OriginY = 180;
        int32 HorizontalSpacing = 320;
        int32 VerticalSpacing = 180;
    };

    static void Layout(UMaterial* Material);
    static void Layout(UMaterial* Material, const FOptions& Options);
    static void Layout(UMaterialFunction* Function);
    static void Layout(UMaterialFunction* Function, const FOptions& Options);
    static void LayoutExpressions(const TArray<UMaterialExpression*>& Expressions);
    static void LayoutExpressions(const TArray<UMaterialExpression*>& Expressions, const FOptions& Options);
};
