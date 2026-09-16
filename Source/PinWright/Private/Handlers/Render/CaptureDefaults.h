// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"

namespace PinWrightRenderCapture
{
    // One stable omitted-size default for every capture renderer. Square is the default, not a
    // constraint: explicit width/height pairs remain supported and drive their own projection.
    constexpr int32 DefaultCaptureEdge = 768;
}
