// Copyright (c) 2026 Alexander Penkin. MIT License.

// EdGraphNodeMeasure.cpp - Offscreen node-widget prepass; see EdGraphNodeMeasure.h.

#include "Layout/EdGraphNodeMeasure.h"

#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "Framework/Application/SlateApplication.h"
#include "Layout/ArrangedChildren.h"
#include "NodeFactory.h"
#include "Rendering/SlateRenderer.h"
#include "SGraphNode.h"
#include "SGraphPin.h"

namespace GraphLayout
{
    namespace EdGraphNodeMeasureImpl
    {
        // Walks the arranged widget tree down to each pin widget and records its vertical centre.
        void CollectPinCentres(const SWidget& Widget, const FGeometry& Geometry,
            const TMap<const SWidget*, const UEdGraphPin*>& PinOf, TMap<const UEdGraphPin*, double>& Out)
        {
            FArrangedChildren Children(EVisibility::Visible);
            Widget.ArrangeChildren(Geometry, Children);
            for (int32 Index = 0; Index < Children.Num(); ++Index)
            {
                const FArrangedWidget& Child = Children[Index];
                if (const UEdGraphPin* const* Pin = PinOf.Find(&Child.Widget.Get()))
                {
                    Out.Add(*Pin, Child.Geometry.GetAbsolutePosition().Y + 0.5 * Child.Geometry.GetAbsoluteSize().Y);
                }
                else
                {
                    CollectPinCentres(Child.Widget.Get(), Child.Geometry, PinOf, Out);
                }
            }
        }
    }

    bool CanMeasureEdGraphNodes()
    {
        return IsInGameThread() && FSlateApplication::IsInitialized() && FSlateApplication::Get().GetRenderer();
    }

    // ponytail: one widget per node per layout call (~widget construction cost each); cache by
    // node if large graphs make the layout pass slow.
    bool MeasureEdGraphNode(UEdGraphNode* Node, FMeasuredNode& Out)
    {
        if (!Node || !CanMeasureEdGraphNodes())
        {
            return false;
        }
        const TSharedPtr<SGraphNode> Widget = FNodeFactory::CreateNodeWidget(Node);
        if (!Widget.IsValid())
        {
            return false;
        }
        Widget->SlatePrepass(1.0f);
        const FVector2D Size(Widget->GetDesiredSize());
        if (Size.X <= 0.0 || Size.Y <= 0.0)
        {
            return false;
        }

        TArray<TSharedRef<SWidget>> PinWidgets;
        Widget->GetPins(PinWidgets);
        TMap<const SWidget*, const UEdGraphPin*> PinOf;
        for (const TSharedRef<SWidget>& PinWidget : PinWidgets)
        {
            if (const UEdGraphPin* Pin = StaticCastSharedRef<SGraphPin>(PinWidget)->GetPinObj())
            {
                PinOf.Add(&PinWidget.Get(), Pin);
            }
        }
        Out.Size = Size;
        Out.PinOffsetY.Reset();
        EdGraphNodeMeasureImpl::CollectPinCentres(*Widget, FGeometry::MakeRoot(Size, FSlateLayoutTransform()), PinOf, Out.PinOffsetY);
        return true;
    }
}
