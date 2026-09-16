// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "Engine/DeveloperSettings.h"
#include "BpirLayoutSettings.generated.h"

// Per-user editor settings for the BPIR compiler's node layout pass.
// Exposed under Project Settings -> Plugins -> "BPIR Layout".

UCLASS(config=EditorPerProjectUserSettings, DefaultConfig, meta=(DisplayName="BPIR Layout"))
class PINWRIGHT_API UBpirLayoutSettings : public UDeveloperSettings
{
    GENERATED_BODY()

public:
    UBpirLayoutSettings();

    /** Kill switch — when false, BPIR compilation skips the layout pass entirely. */
    UPROPERTY(EditAnywhere, config, Category = "BPIR Layout")
    bool bEnableBpirLayoutPass;

    /** Estimated height (in pixels) of a single pin row; used when sizing node bounds. */
    UPROPERTY(EditAnywhere, config, Category = "BPIR Layout|Size Estimation", meta = (ClampMin = "1", UIMin = "1"))
    int32 PinRowHeightPx;

    /** Estimated header height (in pixels) reserved at the top of each node for size estimation. */
    UPROPERTY(EditAnywhere, config, Category = "BPIR Layout|Size Estimation", meta = (ClampMin = "0", UIMin = "0"))
    int32 HeaderHeightPx;

    /** Horizontal interior padding (in pixels) added to estimated node width. */
    UPROPERTY(EditAnywhere, config, Category = "BPIR Layout|Size Estimation", meta = (ClampMin = "0", UIMin = "0"))
    int32 HorizontalPaddingPx;

    /** Horizontal gap (in pixels) between adjacent column bounds. */
    UPROPERTY(EditAnywhere, config, Category = "BPIR Layout|Spacing", meta = (ClampMin = "0", UIMin = "0"))
    int32 NodePadX;

    /** Vertical gap (in pixels) between sibling bounds within a column. */
    UPROPERTY(EditAnywhere, config, Category = "BPIR Layout|Spacing", meta = (ClampMin = "0", UIMin = "0"))
    int32 NodePadY;

    /** Gap (in pixels) between a pure node's right edge and its consumer's left edge. */
    UPROPERTY(EditAnywhere, config, Category = "BPIR Layout|Spacing", meta = (ClampMin = "0", UIMin = "0"))
    int32 PinPadX;

    /** Vertical gap (in pixels) between stacked pure nodes within a single parameter column. */
    UPROPERTY(EditAnywhere, config, Category = "BPIR Layout|Spacing", meta = (ClampMin = "0", UIMin = "0"))
    int32 IntraParameterPadY;

    /** Internal grid (in pixels) used for directional rounding when aligning nodes. */
    UPROPERTY(EditAnywhere, config, Category = "BPIR Layout|Spacing", meta = (ClampMin = "1", UIMin = "1"))
    int32 InternalGridPx;

    /** Maximum iterations allowed in the FormatY pass when resolving collisions. */
    UPROPERTY(EditAnywhere, config, Category = "BPIR Layout|Limits", meta = (ClampMin = "1", UIMin = "1"))
    int32 CollisionIterationCap;

    /** Maximum iterations allowed in the FormatX traversal before bailing out. */
    UPROPERTY(EditAnywhere, config, Category = "BPIR Layout|Limits", meta = (ClampMin = "1", UIMin = "1"))
    int32 TraversalIterationCap;

    virtual FName GetCategoryName() const override { return TEXT("Plugins"); }
    virtual FName GetSectionName() const override { return TEXT("BPIR Layout"); }
    virtual FText GetSectionText() const override;
    virtual FText GetSectionDescription() const override;

    // Persist changed properties immediately when edited in Project Settings.
    virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override
    {
        Super::PostEditChangeProperty(PropertyChangedEvent);
        SaveConfig();
    }
};
