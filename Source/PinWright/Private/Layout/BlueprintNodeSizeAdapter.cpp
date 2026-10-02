// Copyright (c) 2026 Alexander Penkin. MIT License.

// BlueprintNodeSizeAdapter.cpp - Estimated UEdGraphNode sizes and pin rows for graph layout.

#include "Layout/BlueprintNodeSizeAdapter.h"

#include "BpirLayoutSettings.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Fonts/FontMeasure.h"
#include "Framework/Application/SlateApplication.h"
#include "K2Node.h"
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
        // Each title line past the first (a subtitle) deepens the header by this much.
        constexpr double TitleLineHeight = 16.0;
        // The advanced-pin expander and the "Development Only" bar each add this much height.
        constexpr double FooterBarHeight = 20.0;
        // Padding around a default-value box, and the widest such box counted.
        constexpr double ValueBoxPadding = 16.0;
        constexpr double MaxValueBoxWidth = 240.0;
        // Compact nodes: the centre glyph's width, the minimum width, and the vertical slack.
        constexpr double CompactBodyWidth = 110.0;
        constexpr double CompactMinWidth = 96.0;
        constexpr double CompactSlack = 8.0;
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

        // Width of the default-value box an unlinked data input draws beside its label.
        double ValueBoxWidth(const UEdGraphPin* Pin)
        {
            if (Pin->Direction != EGPD_Input || Pin->LinkedTo.Num() > 0
                || Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec || Pin->bDefaultValueIsIgnored)
            {
                return 0.0;
            }
            const FString Value = Pin->GetDefaultAsString();
            return Value.IsEmpty()
                ? 0.0
                : FMath::Min(MaxValueBoxWidth, TextWidth(Value, TEXT("Graph.Node.PinName"), LabelCharWidth) + ValueBoxPadding);
        }

        bool IsCompact(const UEdGraphNode* Node)
        {
            const UK2Node* K2Node = Cast<UK2Node>(Node);
            return K2Node && K2Node->ShouldDrawCompact();
        }

        // Variable getters draw as a headerless pill: just the pin label, its pin centred
        // (measured: a one-pin getter is 38 px tall and its label plus ~48 px wide).
        bool IsDrawnAsVariable(const UEdGraphNode* Node)
        {
            const UK2Node* K2Node = Cast<UK2Node>(Node);
            return K2Node && K2Node->DrawNodeAsVariable();
        }

        struct FSides
        {
            int32 Inputs = 0;
            int32 Outputs = 0;
            double WidestInput = 0.0;
            double WidestOutput = 0.0;
            double WidestValue = 0.0;
        };

        // Rows and widths of the pins drawn on each side (title-bar pins excluded).
        FSides MeasureSides(const UEdGraphNode* Node)
        {
            FSides Sides;
            for (const UEdGraphPin* Pin : Node->Pins)
            {
                if (!FBlueprintNodeSizeAdapter::IsPinShown(Pin) || FBlueprintNodeSizeAdapter::IsPinInTitle(Pin))
                {
                    continue;
                }
                const double Value = ValueBoxWidth(Pin);
                const double Width = TextWidth(PinLabel(Pin), TEXT("Graph.Node.PinName"), LabelCharWidth) + Value;
                Sides.WidestValue = FMath::Max(Sides.WidestValue, Value);
                if (Pin->Direction == EGPD_Input)
                {
                    ++Sides.Inputs;
                    Sides.WidestInput = FMath::Max(Sides.WidestInput, Width);
                }
                else
                {
                    ++Sides.Outputs;
                    Sides.WidestOutput = FMath::Max(Sides.WidestOutput, Width);
                }
            }
            return Sides;
        }

        double CompactHeight(const FSides& Sides, double RowHeight)
        {
            return FMath::Max(FMath::Max(Sides.Inputs, Sides.Outputs), 1) * RowHeight + CompactSlack;
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

    double FBlueprintNodeSizeAdapter::HeaderDepth(const UEdGraphNode* Node) const
    {
        TArray<FString> Lines;
        Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString().ParseIntoArrayLines(Lines);
        return Settings.HeaderHeightPx + FMath::Max(Lines.Num() - 1, 0) * BlueprintNodeSizing::TitleLineHeight;
    }

    double FBlueprintNodeSizeAdapter::PinOffsetY(const UEdGraphPin* Pin) const
    {
        using namespace BlueprintNodeSizing;

        const UEdGraphNode* Node = Pin ? Pin->GetOwningNodeUnchecked() : nullptr;
        if (!Node)
        {
            return 0.0;
        }
        if (IsPinInTitle(Pin))
        {
            return 0.5 * Settings.HeaderHeightPx;
        }
        // Row = shown pins on Pin's side before it; SideCount = all shown pins on that side.
        int32 Row = 0;
        int32 SideCount = 0;
        bool bPassedPin = false;
        for (const UEdGraphPin* Other : Node->Pins)
        {
            bPassedPin |= Other == Pin;
            if (Other && Other->Direction == Pin->Direction && IsPinShown(Other) && !IsPinInTitle(Other))
            {
                Row += bPassedPin ? 0 : 1;
                ++SideCount;
            }
        }
        const double RowHeight = Settings.PinRowHeightPx;
        if (IsCompact(Node) || IsDrawnAsVariable(Node))
        {
            // Compact pure nodes and variable getters centre each side's pins vertically.
            const double Height = CompactHeight(MeasureSides(Node), RowHeight);
            return 0.5 * (Height - SideCount * RowHeight) + (Row + 0.5) * RowHeight;
        }
        return HeaderDepth(Node) + (Row + 0.5) * RowHeight;
    }

    FVector2D FBlueprintNodeSizeAdapter::EstimateNodeSize(const UObject* Object) const
    {
        using namespace BlueprintNodeSizing;

        const UEdGraphNode* Node = Cast<UEdGraphNode>(Object);
        if (!Node)
        {
            return FVector2D(MinWidth, MinHeight);
        }

        const FSides Sides = MeasureSides(Node);
        if (IsDrawnAsVariable(Node))
        {
            return FVector2D(
                FMath::Max(CompactMinWidth, Sides.WidestInput + Sides.WidestOutput + 2.0 * Settings.HorizontalPaddingPx),
                CompactHeight(Sides, Settings.PinRowHeightPx));
        }
        if (IsCompact(Node))
        {
            return FVector2D(
                FMath::Max(CompactMinWidth, CompactBodyWidth + Sides.WidestValue),
                CompactHeight(Sides, Settings.PinRowHeightPx));
        }

        TArray<FString> TitleLines;
        Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString().ParseIntoArrayLines(TitleLines);
        double Title = 0.0;
        for (const FString& Line : TitleLines)
        {
            Title = FMath::Max(Title, TextWidth(Line, TEXT("Graph.Node.NodeTitle"), TitleCharWidth));
        }

        bool bHasAdvancedPins = false;
        for (const UEdGraphPin* Pin : Node->Pins)
        {
            bHasAdvancedPins |= Pin && !Pin->bHidden && Pin->bAdvancedView;
        }
        const double Footer = (bHasAdvancedPins && Node->AdvancedPinDisplay != ENodeAdvancedPins::NoPins ? FooterBarHeight : 0.0)
            + (Node->GetDesiredEnabledState() == ENodeEnabledState::DevelopmentOnly ? FooterBarHeight : 0.0);

        const double Body = Sides.WidestInput + Sides.WidestOutput + CentreGutter;
        const double Width = FMath::Max(MinWidth, FMath::Max(Title, Body) + 2.0 * Settings.HorizontalPaddingPx);
        const int32 Rows = FMath::Max(Sides.Inputs, Sides.Outputs);
        const double Height = FMath::Max(MinHeight, HeaderDepth(Node) + (Rows + 0.5) * Settings.PinRowHeightPx + Footer);
        return FVector2D(Width, Height);
    }
}
