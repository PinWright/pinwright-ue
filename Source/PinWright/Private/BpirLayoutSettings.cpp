// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "BpirLayoutSettings.h"

UBpirLayoutSettings::UBpirLayoutSettings()
    : bEnableBpirLayoutPass(true)
    , PinRowHeightPx(22)
    , HeaderHeightPx(44)
    , HorizontalPaddingPx(24)
    , ColumnGapPx(80)
    , RowGapPx(48)
    , DataColumnGapPx(32)
    , GridSnapPx(8)
{
}

FText UBpirLayoutSettings::GetSectionText() const
{
    return NSLOCTEXT("PinWright", "BpirLayoutSettingsSection", "BPIR Layout");
}

FText UBpirLayoutSettings::GetSectionDescription() const
{
    return NSLOCTEXT("PinWright", "BpirLayoutSettingsDescription",
        "Tuning knobs for the BPIR compiler's node layout pass (spacing, grid, size estimation).");
}
