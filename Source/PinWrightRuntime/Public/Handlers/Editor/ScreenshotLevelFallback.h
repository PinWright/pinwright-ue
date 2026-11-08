// Copyright (c) 2026 Alexander Penkin. MIT License.

// Seam for editor.screenshot (runtime ViewportHandler.cpp): with no game/PIE viewport the verb
// captures the active level-editor viewport, which needs UnrealEd / LevelEditor. The editor
// module's ViewportHandler.cpp installs the capture at static init; a game process has none, and
// the verb then answers NO_VIEWPORT.
#pragma once

#include "CoreMinimal.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/Render/CaptureExposure.h"

namespace PinWrightScreenshotLevelFallback
{
    // Captures synchronously and calls OnComplete exactly once (same payload contract as the
    // game-viewport branch: width, height, path, captureSource, fixedSize, viewport).
    using FCaptureFn = void (*)(const FString& RequestedFilename, const FIntPoint& RequestedSize,
        const PinWrightRenderCapture::FExposurePin& Exposure, const TOptional<FVector>& AimLocation,
        const TOptional<FRotator>& AimRotation, const FJobOnComplete& OnComplete);

    PINWRIGHTRUNTIME_API void Set(FCaptureFn Fn);
    // Null in a game process (and in the editor before the editor module loads).
    PINWRIGHTRUNTIME_API FCaptureFn Get();
}
