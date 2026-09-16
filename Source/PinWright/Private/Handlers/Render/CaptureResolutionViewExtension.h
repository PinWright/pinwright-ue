// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "SceneViewExtension.h"

class FViewport;

namespace PinWrightRenderCapture
{
    struct FViewportCaptureOutput;

    class FCaptureResolutionViewExtension final : public FSceneViewExtensionBase
    {
    public:
        FCaptureResolutionViewExtension(const FAutoRegister& AutoRegister, FViewport* InViewport);

        // Pure virtual through UE 5.5 (ISceneViewExtension gained empty defaults in 5.6). This
        // extension only measures per-view resolution state, so there is nothing to do here.
        virtual void SetupViewFamily(FSceneViewFamily& InViewFamily) override {}
        virtual void SetupView(FSceneViewFamily& InViewFamily, FSceneView& InView) override;
        virtual void BeginRenderViewFamily(FSceneViewFamily& InViewFamily) override;
        virtual int32 GetPriority() const override;

        bool PopulateCaptureOutput(FViewportCaptureOutput& OutCapture) const;

    protected:
        virtual bool IsActiveThisFrame_Internal(
            const FSceneViewExtensionContext& Context) const override;

    private:
        FViewport* Viewport = nullptr;
        bool bObserved = false;
        int32 RenderWidth = 0;
        int32 RenderHeight = 0;
        float PrimaryResolutionFraction = 0.0f;
        float SecondaryResolutionFraction = 0.0f;
        float ResolutionFraction = 0.0f;
    };
}
