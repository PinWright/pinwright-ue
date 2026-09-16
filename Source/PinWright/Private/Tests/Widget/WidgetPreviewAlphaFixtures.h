// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

// Fixtures shared by the two test files that assert the Designer-preview alpha facts on a
// verb response -- Tests/Widget/TestWidgetDesignerAlphaResponse.cpp (widget.screenshot_designer)
// and Tests/Utility/TestAssetDumpWidgetPreviewAlpha.cpp (asset.dump's widget aspect). Named
// namespace with inline functions rather than a per-file anonymous namespace: both files can
// land in the same Unity blob, where duplicate anonymous-namespace helpers are an ODR
// collision (plugin CLAUDE.md, Build rules).
//
// The fixture exists to make one number falsifiable. A capture of a uniformly transparent
// widget reports alphaZeroFraction ~1.0 and a uniformly opaque one reports ~0.0 -- both of
// which a handler that emitted a literal could also produce. Half-covering the canvas puts the
// honest answer near 0.5, where neither literal lands.

#include "CoreMinimal.h"

#include "Tests/TestUtils.h"
#include "Tests/Widget/WidgetTestFixtures.h"

#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetTree.h"
#include "Brushes/SlateColorBrush.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/Image.h"
#include "Editor.h"
#include "Layout/Margin.h"
#include "Widgets/Layout/Anchors.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "UObject/Package.h"
#include "WidgetBlueprint.h"

namespace WidgetPreviewAlphaFixtures
{
    // 400x200 design-time preview. Both consumers render it at a pinned max_size so the
    // handler's capture and any cross-check capture rasterise the same pixel count; two
    // fractions measured over different rasterisations cannot be compared tightly.
    inline constexpr double PinnedDesignWidth = 400.0;
    inline constexpr double PinnedDesignHeight = 200.0;
    // asset.dump's widget aspect hardcodes MaxSize 1024 (AssetDumpHandler.cpp, the WBP
    // branch), and widget.screenshot_designer defaults max_size to 1024. Matching them keeps a
    // cross-verb comparison meaningful.
    inline constexpr int32 PinnedMaxSize = 1024;

    // The Designer preview canvas is sized from the WBP CDO's DesignSizeMode / DesignTimeSize
    // pair; Custom + an explicit size pins it so the capture's pixel count does not depend on
    // monitor DPI or panel layout. Same mechanism TestWidgetDesignerScreenshotHandler.cpp's
    // PreviewMatchesCanvasBounds uses; reimplemented here because that helper is file-local to
    // a test this chunk does not own.
    inline void PinDesignerPreviewSize(UWidgetBlueprint* WBP, FVector2D Size)
    {
        if (!WBP || !WBP->GeneratedClass)
        {
            return;
        }
        if (UUserWidget* CDO = Cast<UUserWidget>(WBP->GeneratedClass->GetDefaultObject()))
        {
            CDO->DesignSizeMode = EDesignPreviewSizeMode::Custom;
            CDO->DesignTimeSize = Size;
        }
    }

    // Covers the TOP HALF of the canvas with an opaque white fill and leaves the bottom half at
    // the render target's transparent clear, so a correct pre-stamp measurement lands near 0.5.
    //
    // FSlateColorBrush is a brush with NO resource object and a tint; Slate renders that with
    // its default white texture, which is how solid fills are drawn throughout the editor. If
    // it ever stops rendering, the measured fraction goes to ~1.0 and the band assertions in
    // both consumers fail with the fixture named in their message -- read that as a fixture
    // defect, not a handler defect.
    inline UImage* AddTopHalfOpaqueFill(UWidgetBlueprint* WBP)
    {
        if (!WBP || !WBP->WidgetTree)
        {
            return nullptr;
        }
        UCanvasPanel* RootCanvas = Cast<UCanvasPanel>(WBP->WidgetTree->RootWidget);
        if (!RootCanvas)
        {
            return nullptr;
        }
        UImage* Fill = WBP->WidgetTree->ConstructWidget<UImage>(
            UImage::StaticClass(), TEXT("OpaqueTopHalf"));
        if (!Fill)
        {
            return nullptr;
        }
        Fill->SetBrush(FSlateColorBrush(FLinearColor::White));
        Fill->SetColorAndOpacity(FLinearColor::White);
        if (UCanvasPanelSlot* CanvasSlot = Cast<UCanvasPanelSlot>(RootCanvas->AddChild(Fill)))
        {
            CanvasSlot->SetAnchors(FAnchors(0.0f, 0.0f, 1.0f, 0.5f));
            CanvasSlot->SetOffsets(FMargin(0.0f));
            CanvasSlot->SetAlignment(FVector2D(0.0, 0.0));
        }
        WidgetTestFixtures::RegisterWidgetVariable(WBP, Fill->GetFName());
        return Fill;
    }

    // A correct pre-stamp measurement over AddTopHalfOpaqueFill's canvas lands near 0.5. The
    // band is wide because glyph-free antialiasing at the fill's edge and the exact preview
    // rounding both move it slightly; it is narrow enough that 0.0 (the field never copied out
    // of FCaptureInfo) and 1.0 (a "the whole preview was transparent" guess) are both rejected.
    inline constexpr double HalfCoveredFractionMin = 0.20;
    inline constexpr double HalfCoveredFractionMax = 0.80;

    inline bool IsHalfCoveredFraction(double Fraction)
    {
        return Fraction > HalfCoveredFractionMin && Fraction < HalfCoveredFractionMax;
    }

    inline void CloseAndCleanupWidget(UWidgetBlueprint* WBP, const FString& AssetPath)
    {
        if (GEditor)
        {
            if (UAssetEditorSubsystem* Subsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>())
            {
                if (WBP)
                {
                    Subsystem->CloseAllEditorsForAsset(WBP);
                }
            }
        }
        if (WBP)
        {
            if (UPackage* Package = WBP->GetOutermost())
            {
                Package->SetDirtyFlag(false);
            }
        }
        CleanupTestAsset(AssetPath);
    }
}
