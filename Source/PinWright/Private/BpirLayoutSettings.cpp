// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "BpirLayoutSettings.h"

// Defaults mirror the pre-settings hardcoded values in the BPIR layout pass.

UBpirLayoutSettings::UBpirLayoutSettings()
    : bEnableBpirLayoutPass(true)
    , PinRowHeightPx(22)
    , HeaderHeightPx(44)
    , HorizontalPaddingPx(24)
    , NodePadX(80)
    , NodePadY(48)
    , PinPadX(32)
    , IntraParameterPadY(16)
    , InternalGridPx(8)
    , CollisionIterationCap(30)
    , TraversalIterationCap(10000)
{
}

FText UBpirLayoutSettings::GetSectionText() const
{
    return NSLOCTEXT("PinWright", "BpirLayoutSettingsSection", "BPIR Layout");
}

FText UBpirLayoutSettings::GetSectionDescription() const
{
    return NSLOCTEXT("PinWright", "BpirLayoutSettingsDescription",
        "Tuning knobs for the BPIR compiler's node layout pass (spacing, size estimation, iteration caps).");
}
