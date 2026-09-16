// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Handlers/Render/PreviewViewportCaptureUtils.h"

class FHandlerContext;
class FJsonObject;
class UWorld;

namespace PinWrightOpenLevelCapture
{
    struct FWorldMatchDecision
    {
        bool bAllowed = false;
        bool bViewportIsPieWorld = false;
        bool bMapsMatch = false;
        FString ErrorCode;
        FString Reason;
        FString EditorMapName;
        FString ViewportMapName;
    };

    using FSuccessDecorator = TFunction<void(
        const PinWrightRenderCapture::FViewportCaptureOutput&,
        const TSharedPtr<FJsonObject>&)>;

    FWorldMatchDecision EvaluateViewportWorldMatch(
        const UWorld* EditorWorld, const UWorld* ViewportWorld, bool bAllowPieWorld);

    bool Handle(FHandlerContext& Ctx);
    bool Handle(FHandlerContext& Ctx,
        const PinWrightRenderCapture::FViewportCaptureHooks& Hooks,
        const FSuccessDecorator& DecorateSuccess);
}
