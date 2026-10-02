// Copyright (c) 2026 Alexander Penkin. MIT License.

// PwGraphLayoutMaterial.h - PwGraphLayout adapter for material and material-function expression
// graphs.
//
// Every wire is data. A material's output node (at Material->EditorX/Y) is a fixed root, so the
// graph grows leftwards from it; a function's outputs are sinks and root their own trees. Only
// expressions still at (0,0) move: positioned expressions are fixed obstacles. Sizes are estimated
// from the caption, the pin counts and an open preview. Comment boxes (the editor comments) are
// model comments: a comment around positioned expressions is an obstacle the moved expressions
// stay out of; one with a moved member is re-fitted.

#pragma once

#include "CoreMinimal.h"
#include "Layout/PwGraphLayout.h"

class UMaterial;
class UMaterialExpression;
class UMaterialExpressionComment;
class UMaterialFunction;

namespace PwGraphLayout
{
    struct FMaterialModel
    {
        FLayoutGraph Layout;
        // Parallel to the first Expressions.Num() model nodes, ordered by expression GUID. A
        // material's output node, when built, is the one model node past the end.
        TArray<UMaterialExpression*> Expressions;
        // Parallel to Layout.Comments, ordered by comment key.
        TArray<UMaterialExpressionComment*> Comments;
    };

    // Exactly one of Material / Function is used (Material wins when both are given).
    PINWRIGHT_API FMaterialModel BuildMaterialModel(UMaterial* Material, UMaterialFunction* Function);

    // Writes positions and re-fitted comment rects through Modify(), so an enclosing transaction
    // undoes them.
    PINWRIGHT_API FArrangeReport ArrangeMaterial(UMaterial* Material);
    PINWRIGHT_API FArrangeReport ArrangeMaterialFunction(UMaterialFunction* Function);
}
