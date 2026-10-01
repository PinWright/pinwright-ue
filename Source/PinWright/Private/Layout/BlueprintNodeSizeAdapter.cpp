// Copyright (c) 2026 Alexander Penkin. MIT License.

// BlueprintNodeSizeAdapter.cpp - Estimated UEdGraphNode sizes and pin rows for graph layout.

#include "Layout/BlueprintNodeSizeAdapter.h"

#include "BpirLayoutSettings.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "Fonts/FontMeasure.h"
#include "Framework/Application/SlateApplication.h"
#include "K2Node_Event.h"
#include "Rendering/SlateRenderer.h"
#include "Styling/AppStyle.h"

namespace GraphLayout
{
    namespace BlueprintNodeSizing
    {
        constexpr double MinWidth = 160.0;
        constexpr double MinHeight = 64.0;
        // Space between the input label column and the output label column.
        constexpr double CentreGutter = 40.0;
        // Character widths used when no Slate renderer can measure text (headless runs).
        constexpr double TitleCharWidth = 8.0;
        constexpr double LabelCharWidth = 7.0;

        double TextWidth(const FString& Text, const TCHAR* FontStyle, double CharWidth)
        {
            if (Text.IsEmpty())
            {
                return 0.0;
            }
            if (FSlateApplication::IsInitialized() && FSlateApplication::Get().GetRenderer())
            {
                const FSlateFontInfo Font = FAppStyle::Get().GetFontStyle(FontStyle);
                return FSlateApplication::Get().GetRenderer()->GetFontMeasureService()->Measure(Text, Font).X;
            }
            return Text.Len() * CharWidth;
        }

        FString PinLabel(const UEdGraphPin* Pin)
        {
            const FString Display = Pin->GetDisplayName().ToString();
            return Display.IsEmpty() ? Pin->PinName.ToString() : Display;
        }
    }

    bool FBlueprintNodeSizeAdapter::IsPinShown(const UEdGraphPin* Pin)
    {
        if (!Pin || Pin->bHidden)
        {
            return false;
        }
        const UEdGraphNode* Owner = Pin->GetOwningNodeUnchecked();
        return !(Pin->bAdvancedView && Owner && Owner->AdvancedPinDisplay == ENodeAdvancedPins::Hidden);
    }

    bool FBlueprintNodeSizeAdapter::IsPinInTitle(const UEdGraphPin* Pin)
    {
        return Pin && Pin->PinName == UK2Node_Event::DelegateOutputName
            && Pin->GetOwningNodeUnchecked() && Pin->GetOwningNodeUnchecked()->IsA<UK2Node_Event>();
    }

    double FBlueprintNodeSizeAdapter::TitlePinOffsetY() const
    {
        return 0.5 * Settings.HeaderHeightPx;
    }

    double FBlueprintNodeSizeAdapter::PinOffsetY(int32 Row) const
    {
        return Settings.HeaderHeightPx + (Row + 0.5) * Settings.PinRowHeightPx;
    }

    FVector2D FBlueprintNodeSizeAdapter::EstimateNodeSize(const UObject* Object) const
    {
        using namespace BlueprintNodeSizing;

        const UEdGraphNode* Node = Cast<UEdGraphNode>(Object);
        if (!Node)
        {
            return FVector2D(MinWidth, MinHeight);
        }

        int32 InputRows = 0;
        int32 OutputRows = 0;
        double WidestInput = 0.0;
        double WidestOutput = 0.0;
        for (const UEdGraphPin* Pin : Node->Pins)
        {
            if (!IsPinShown(Pin) || IsPinInTitle(Pin))
            {
                continue;
            }
            const double LabelWidth = TextWidth(PinLabel(Pin), TEXT("Graph.Node.PinName"), LabelCharWidth);
            if (Pin->Direction == EGPD_Input)
            {
                ++InputRows;
                WidestInput = FMath::Max(WidestInput, LabelWidth);
            }
            else
            {
                ++OutputRows;
                WidestOutput = FMath::Max(WidestOutput, LabelWidth);
            }
        }

        const double Title = TextWidth(
            Node->GetNodeTitle(ENodeTitleType::ListView).ToString(), TEXT("Graph.Node.NodeTitle"), TitleCharWidth);
        const double Body = WidestInput + WidestOutput + CentreGutter;
        const double Width = FMath::Max(MinWidth, FMath::Max(Title, Body) + 2.0 * Settings.HorizontalPaddingPx);
        const int32 Rows = FMath::Max(InputRows, OutputRows);
        const double Height = FMath::Max(MinHeight, Settings.HeaderHeightPx + (Rows + 0.5) * Settings.PinRowHeightPx);
        return FVector2D(Width, Height);
    }
}
