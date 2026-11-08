// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Utils/RenderingAvailability.h"

int32& PinWrightRendering::ForcedUnavailableDepth()
{
    static int32 Depth = 0;
    return Depth;
}
