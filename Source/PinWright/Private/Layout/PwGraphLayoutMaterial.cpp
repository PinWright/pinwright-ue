// Copyright (c) 2026 Alexander Penkin. MIT License.

// PwGraphLayoutMaterial.cpp - Material expression adapter for PwGraphLayout.

#include "Layout/PwGraphLayoutMaterial.h"

#include "MGIR/MGIRExpressionUtils.h"
#include "Material/MaterialInputIterCompat.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionComment.h"
#include "Materials/MaterialExpressionNamedReroute.h"
#include "Materials/MaterialFunction.h"

namespace PwGraphLayoutMaterialImpl
{
    using namespace PwGraphLayout;

    constexpr double HeaderHeight = 32.0;
    constexpr double RowHeight = 24.0;
    constexpr double MinWidth = 144.0;
    constexpr double MinHeight = 64.0;
    constexpr double CaptionCharWidth = 8.0;
    constexpr double SidePadding = 48.0;

    // Material layout spacing: wider data columns than Blueprint, since every node is data.
    FSpacing MaterialSpacing()
    {
        FSpacing Spacing;
        Spacing.ColumnGap = 80.0;
        Spacing.RowGap = 32.0;
        Spacing.DataColumnGap = 64.0;
        Spacing.Grid = 16.0;
        return Spacing;
    }

    FString ExpressionKey(const UMaterialExpression* Expression)
    {
        return MGIRExpressionUtils::StableExpressionKey(Expression) + TEXT(":") + Expression->GetName();
    }

    double RowCentre(int32 Row)
    {
        return HeaderHeight + (Row + 0.5) * RowHeight;
    }

    FVector2D EstimateSize(const FString& Caption, int32 Rows)
    {
        return FVector2D(
            FMath::Max(MinWidth, Caption.Len() * CaptionCharWidth + SidePadding),
            FMath::Max(MinHeight, HeaderHeight + (FMath::Max(Rows, 1) + 0.5) * RowHeight));
    }

    // Input rows first, then output rows, as model pin slots.
    void AddRows(FLayoutNode& Node, int32 Inputs, int32 Outputs)
    {
        for (int32 Row = 0; Row < Inputs; ++Row)
        {
            Node.Pins.Add({ EPinSide::Input, EWireKind::Data, RowCentre(Row) });
        }
        for (int32 Row = 0; Row < Outputs; ++Row)
        {
            Node.Pins.Add({ EPinSide::Output, EWireKind::Data, RowCentre(Row) });
        }
    }

    FArrangeReport ArrangeAndWrite(UMaterial* Material, UMaterialFunction* Function)
    {
        FMaterialModel Model = BuildMaterialModel(Material, Function);
        const FArrangeReport Report = Arrange(Model.Layout, MaterialSpacing());
        for (int32 Index = 0; Index < Model.Expressions.Num(); ++Index)
        {
            UMaterialExpression* Expression = Model.Expressions[Index];
            const FLayoutNode& Placed = Model.Layout.Nodes[Index];
            const int32 X = FMath::RoundToInt(Placed.Position.X);
            const int32 Y = FMath::RoundToInt(Placed.Position.Y);
            if (Placed.bMovable
                && (Expression->MaterialExpressionEditorX != X || Expression->MaterialExpressionEditorY != Y))
            {
                Expression->Modify();
                Expression->MaterialExpressionEditorX = X;
                Expression->MaterialExpressionEditorY = Y;
            }
        }
        return Report;
    }
}

namespace PwGraphLayout
{
    FMaterialModel BuildMaterialModel(UMaterial* Material, UMaterialFunction* Function)
    {
        using namespace PwGraphLayoutMaterialImpl;

        FMaterialModel Model;
        TArray<UMaterialExpression*> All;
        if (Material)
        {
            MGIRExpressionUtils::CopyMaterialExpressions(Material, All);
        }
        else
        {
            MGIRExpressionUtils::CopyFunctionExpressions(Function, All);
        }
        for (UMaterialExpression* Expression : All)
        {
            if (!Expression->IsA<UMaterialExpressionComment>())
            {
                Model.Expressions.Add(Expression);
            }
        }
        Model.Expressions.Sort([](const UMaterialExpression& A, const UMaterialExpression& B)
        {
            return ExpressionKey(&A) < ExpressionKey(&B);
        });

        TMap<const UMaterialExpression*, int32> IndexOf;
        TArray<int32> FirstOutputSlot;
        for (int32 Index = 0; Index < Model.Expressions.Num(); ++Index)
        {
            UMaterialExpression* Expression = Model.Expressions[Index];
            IndexOf.Add(Expression, Index);

            int32 Inputs = 0;
            ForEachExpressionInput(Expression, [&Inputs](FExpressionInput*, int32) -> bool { ++Inputs; return false; });
            // A named-reroute usage reads its declaration through a pointer; give that link a row.
            if (Expression->IsA<UMaterialExpressionNamedRerouteUsage>())
            {
                ++Inputs;
            }
            const int32 Outputs = Expression->GetOutputs().Num();

            TArray<FString> Captions;
            Expression->GetCaption(Captions);
            FLayoutNode& Node = Model.Layout.Nodes.AddDefaulted_GetRef();
            Node.Key = ExpressionKey(Expression);
            Node.Position = FVector2D(Expression->MaterialExpressionEditorX, Expression->MaterialExpressionEditorY);
            Node.Size = EstimateSize(Captions.Num() > 0 ? Captions[0] : Expression->GetClass()->GetName(),
                FMath::Max(Inputs, Outputs));
            Node.bMovable = Expression->MaterialExpressionEditorX == 0 && Expression->MaterialExpressionEditorY == 0;
            AddRows(Node, Inputs, Outputs);
            FirstOutputSlot.Add(Inputs);
        }

        auto AddWireFrom = [&Model, &IndexOf, &FirstOutputSlot](const FExpressionInput& Input, int32 ToNode, int32 ToSlot)
        {
            const int32* From = Input.Expression ? IndexOf.Find(Input.Expression) : nullptr;
            if (!From)
            {
                return;
            }
            const int32 Outputs = Model.Layout.Nodes[*From].Pins.Num() - FirstOutputSlot[*From];
            if (Outputs > 0)
            {
                const int32 Output = FMath::Clamp(Input.OutputIndex, 0, Outputs - 1);
                Model.Layout.Wires.Add({ *From, FirstOutputSlot[*From] + Output, ToNode, ToSlot });
            }
        };

        for (int32 Index = 0; Index < Model.Expressions.Num(); ++Index)
        {
            UMaterialExpression* Expression = Model.Expressions[Index];
            ForEachExpressionInput(Expression, [&AddWireFrom, Index](FExpressionInput* Input, int32 Row) -> bool
            {
                AddWireFrom(*Input, Index, Row);
                return false;
            });
            if (const UMaterialExpressionNamedRerouteUsage* Usage = Cast<UMaterialExpressionNamedRerouteUsage>(Expression))
            {
                const int32* Declaration = Usage->Declaration ? IndexOf.Find(Usage->Declaration.Get()) : nullptr;
                if (Declaration && FirstOutputSlot[*Declaration] < Model.Layout.Nodes[*Declaration].Pins.Num())
                {
                    Model.Layout.Wires.Add({ *Declaration, FirstOutputSlot[*Declaration], Index, FirstOutputSlot[Index] - 1 });
                }
            }
        }

        if (Material)
        {
            // The output node: one input row per connected material property, in property order.
            TArray<const FExpressionInput*> Connected;
            for (int32 Property = 0; Property < MP_MAX; ++Property)
            {
                const FExpressionInput* Input = Material->GetExpressionInputForProperty(static_cast<EMaterialProperty>(Property));
                if (Input && Input->Expression)
                {
                    Connected.AddUnique(Input);
                }
            }
            const int32 Root = Model.Layout.Nodes.Num();
            FLayoutNode& Node = Model.Layout.Nodes.AddDefaulted_GetRef();
            Node.Key = TEXT("~MaterialOutput");
            Node.Position = FVector2D(Material->EditorX, Material->EditorY);
            Node.Size = EstimateSize(Material->GetName(), Connected.Num());
            Node.bMovable = false;
            AddRows(Node, Connected.Num(), 0);
            FirstOutputSlot.Add(Connected.Num());
            for (int32 Row = 0; Row < Connected.Num(); ++Row)
            {
                AddWireFrom(*Connected[Row], Root, Row);
            }
            Model.Layout.Roots.Add(Root);
        }
        return Model;
    }

    FArrangeReport ArrangeMaterial(UMaterial* Material)
    {
        return Material ? PwGraphLayoutMaterialImpl::ArrangeAndWrite(Material, nullptr) : FArrangeReport();
    }

    FArrangeReport ArrangeMaterialFunction(UMaterialFunction* Function)
    {
        return Function ? PwGraphLayoutMaterialImpl::ArrangeAndWrite(nullptr, Function) : FArrangeReport();
    }
}
