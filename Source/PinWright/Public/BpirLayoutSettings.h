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

    /** Measure node sizes and pin rows from the nodes' editor widgets (offscreen Slate prepass) when Slate is running; when false, or without Slate, sizes are estimated. */
    UPROPERTY(EditAnywhere, config, Category = "BPIR Layout|Size Estimation")
    bool bMeasureNodeSizes;

    /** Estimated height (in pixels) of a single pin row; used when sizing node bounds. */
    UPROPERTY(EditAnywhere, config, Category = "BPIR Layout|Size Estimation", meta = (ClampMin = "1", UIMin = "1"))
    int32 PinRowHeightPx;

    /** Estimated header height (in pixels) reserved at the top of each node for size estimation. */
    UPROPERTY(EditAnywhere, config, Category = "BPIR Layout|Size Estimation", meta = (ClampMin = "0", UIMin = "0"))
    int32 HeaderHeightPx;

    /** Horizontal interior padding (in pixels) added to estimated node width. */
    UPROPERTY(EditAnywhere, config, Category = "BPIR Layout|Size Estimation", meta = (ClampMin = "0", UIMin = "0"))
    int32 HorizontalPaddingPx;

    /** Horizontal gap (in pixels) between a flow (exec) node and the next flow node or its data block. */
    UPROPERTY(EditAnywhere, config, Category = "BPIR Layout|Spacing", meta = (ClampMin = "0", UIMin = "0"))
    int32 ColumnGapPx;

    /** Vertical clearance (in pixels) between any two nodes that share an X range. */
    UPROPERTY(EditAnywhere, config, Category = "BPIR Layout|Spacing", meta = (ClampMin = "0", UIMin = "0"))
    int32 RowGapPx;

    /** Gap (in pixels) between data (pure) node columns, and between a data column and its consumer. */
    UPROPERTY(EditAnywhere, config, Category = "BPIR Layout|Spacing", meta = (ClampMin = "0", UIMin = "0"))
    int32 DataColumnGapPx;

    /** Grid (in pixels) node positions snap to. */
    UPROPERTY(EditAnywhere, config, Category = "BPIR Layout|Spacing", meta = (ClampMin = "1", UIMin = "1"))
    int32 GridSnapPx;

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
