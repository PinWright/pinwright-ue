// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "MGIR/MGIRLayoutEngine.h"


#include "MGIR/MGIRExpressionUtils.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialFunction.h"

namespace
{
bool IsUnpositioned(const UMaterialExpression* Expression)
{
    return Expression
        && Expression->MaterialExpressionEditorX == 0
        && Expression->MaterialExpressionEditorY == 0;
}

}

void FMGIRLayoutEngine::Layout(UMaterial* Material, const FOptions& Options)
{
    TArray<UMaterialExpression*> Expressions;
    MGIRExpressionUtils::CopyMaterialExpressions(Material, Expressions);
    LayoutExpressions(Expressions, Options);
}

void FMGIRLayoutEngine::Layout(UMaterial* Material)
{
    Layout(Material, FOptions());
}

void FMGIRLayoutEngine::Layout(UMaterialFunction* Function, const FOptions& Options)
{
    TArray<UMaterialExpression*> Expressions;
    MGIRExpressionUtils::CopyFunctionExpressions(Function, Expressions);
    LayoutExpressions(Expressions, Options);
}

void FMGIRLayoutEngine::Layout(UMaterialFunction* Function)
{
    Layout(Function, FOptions());
}

void FMGIRLayoutEngine::LayoutExpressions(const TArray<UMaterialExpression*>& Expressions)
{
    LayoutExpressions(Expressions, FOptions());
}

void FMGIRLayoutEngine::LayoutExpressions(const TArray<UMaterialExpression*>& Expressions, const FOptions& Options)
{
    TArray<MGIRExpressionUtils::FExpressionSortRecord> Records =
        MGIRExpressionUtils::BuildExpressionSortRecords(Expressions);

    TMap<int32, int32> LanesByDepth;
    for (const MGIRExpressionUtils::FExpressionSortRecord& Record : Records)
    {
        UMaterialExpression* Expression = Record.Expression;
        if (!IsUnpositioned(Expression))
        {
            continue;
        }

        const int32 Depth = Record.Depth;
        int32& Lane = LanesByDepth.FindOrAdd(Depth);
        Expression->MaterialExpressionEditorX = Options.OriginX + Depth * Options.HorizontalSpacing;
        Expression->MaterialExpressionEditorY = Options.OriginY + Lane * Options.VerticalSpacing;
        ++Lane;
    }
}
