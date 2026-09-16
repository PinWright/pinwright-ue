// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/Render/CaptureResolutionViewExtension.h"

#include "Handlers/Render/PreviewViewportCaptureUtils.h"

#include "Compat/EngineVersionCompat.h"
#include "DynamicResolutionState.h"
#include "SceneView.h"
#include "UnrealClient.h"

namespace PinWrightRenderCapture
{
FCaptureResolutionViewExtension::FCaptureResolutionViewExtension(
    const FAutoRegister& AutoRegister, FViewport* InViewport)
    : FSceneViewExtensionBase(AutoRegister)
    , Viewport(InViewport)
{
}

void FCaptureResolutionViewExtension::SetupView(
    FSceneViewFamily& InViewFamily, FSceneView& InView)
{
    if (InViewFamily.RenderTarget != Viewport)
    {
        return;
    }

    // These are per-view-family/per-view overrides. They do not mutate the editor client's private
    // PreviewResolutionFraction optional or its low-DPI preference.
    InViewFamily.SecondaryViewFraction = 1.0f;
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 5, 0)
    // FSceneViewInitOptions::OverridePrimaryResolutionFraction arrived in UE 5.5. On 5.4 there is
    // no per-view primary override at all, so the primary fraction is whatever the family's
    // screen-percentage interface reports and only the secondary fraction can be pinned here.
    InView.SceneViewInitOptions.OverridePrimaryResolutionFraction = 1.0f;
#endif

    // A still capture suppresses TemporalAA, so nothing accumulates the stochastic passes across
    // frames. Left alone, the view state's frame index keeps advancing and re-seeds Lumen screen
    // probes, screen-space reflections and AO on every draw, so re-shooting one pose produces a
    // different picture. FrameIndex is the seed those passes read (SceneVisibility.cpp assigns
    // ViewState->FrameIndex from this override), so pin it for the capture window: every draw in
    // a set then samples the same sequence and identical poses render identically.
    InView.OverrideFrameIndexValue = 0;
}

void FCaptureResolutionViewExtension::BeginRenderViewFamily(FSceneViewFamily& InViewFamily)
{
    if (InViewFamily.RenderTarget != Viewport || InViewFamily.Views.Num() == 0)
    {
        return;
    }

    // BeginRenderViewFamily is the last game-thread extension hook before renderer creation. Pin
    // the mutable family fraction again so a preceding extension cannot reintroduce low-DPI
    // scaling. The per-view primary override was applied in SetupView, where views are mutable.
    InViewFamily.SecondaryViewFraction = 1.0f;

    const FSceneView& View = *InViewFamily.Views[0];
    float PrimaryUpperBound = 1.0f;
    if (const ISceneViewFamilyScreenPercentage* ScreenPercentage =
            InViewFamily.GetScreenPercentageInterface())
    {
        const DynamicRenderScaling::TMap<float> UpperBounds =
            ScreenPercentage->GetResolutionFractionsUpperBound();
        PrimaryUpperBound = UpperBounds[GDynamicPrimaryResolutionFraction];
    }

#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 5, 0)
    PrimaryResolutionFraction =
        View.SceneViewInitOptions.OverridePrimaryResolutionFraction > 0.0f
        ? View.SceneViewInitOptions.OverridePrimaryResolutionFraction
        : PrimaryUpperBound;
#else
    // Neither OverridePrimaryResolutionFraction nor OverscanResolutionFraction exists on 5.4,
    // where the family's upper bound is the only primary fraction and nothing overscans.
    PrimaryResolutionFraction = PrimaryUpperBound;
#endif
    SecondaryResolutionFraction = InViewFamily.SecondaryViewFraction;
    ResolutionFraction = PrimaryResolutionFraction * SecondaryResolutionFraction;
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 5, 0)
    const float RasterResolutionFraction = ResolutionFraction *
        View.SceneViewInitOptions.OverscanResolutionFraction;
#else
    const float RasterResolutionFraction = ResolutionFraction;
#endif
    RenderWidth = FMath::CeilToInt(
        static_cast<double>(View.UnscaledViewRect.Width()) * RasterResolutionFraction);
    RenderHeight = FMath::CeilToInt(
        static_cast<double>(View.UnscaledViewRect.Height()) * RasterResolutionFraction);
    bObserved = true;
}

int32 FCaptureResolutionViewExtension::GetPriority() const
{
    // Run after other extensions so the receipt reflects their final view-family state.
    return MIN_int32;
}

bool FCaptureResolutionViewExtension::PopulateCaptureOutput(
    FViewportCaptureOutput& OutCapture) const
{
    if (!bObserved)
    {
        return false;
    }

    OutCapture.RenderWidth = RenderWidth;
    OutCapture.RenderHeight = RenderHeight;
    OutCapture.RenderPrimaryResolutionFraction = PrimaryResolutionFraction;
    OutCapture.RenderSecondaryResolutionFraction = SecondaryResolutionFraction;
    OutCapture.RenderResolutionFraction = ResolutionFraction;
    OutCapture.RenderScreenPercentage = FMath::RoundToInt(ResolutionFraction * 100.0f);
    OutCapture.bRenderResolutionPinned =
        FMath::IsNearlyEqual(PrimaryResolutionFraction, 1.0f) &&
        FMath::IsNearlyEqual(SecondaryResolutionFraction, 1.0f);
    OutCapture.bRenderUpscaled =
        RenderWidth < OutCapture.Width || RenderHeight < OutCapture.Height;
    return true;
}

bool FCaptureResolutionViewExtension::IsActiveThisFrame_Internal(
    const FSceneViewExtensionContext& Context) const
{
    return Context.Viewport == Viewport;
}
}
