// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/Render/SceneCaptureProbeUtils.h"
#include "Handlers/ErrorCodes.h"
#include "Handlers/Render/OrthoTileCaptureUtils.h"
#include "Handlers/Render/PreviewViewportCaptureUtils.h"
#include "Utils/ScreenshotUtils.h"

#include "Camera/CameraTypes.h"
#include "Compat/EngineVersionCompat.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/World.h"
#include "Engine/PostProcessVolume.h"
#include "PixelFormat.h"
#include "RenderingThread.h"
#include "TextureResource.h"
#include "UnrealClient.h"
// GetTransientPackage() returns UPackage*, and NewObject's outer parameter needs the
// UPackage -> UObject conversion, which requires the complete type.
#include "UObject/Package.h"

namespace PinWrightSceneCaptureProbe
{
    bool SourceIsSingleChannelDepth(ESceneCaptureSource Source)
    {
        return Source == SCS_SceneDepth;
    }

    FSceneCaptureProbe::FSceneCaptureProbe(UWorld* InWorld)
        : World(InWorld)
    {
        if (!InWorld)
        {
            return;
        }

        // Ownerless + transient + outered to the transient package: nothing about this
        // component reaches the level, its actor list, its package dirty state, or the
        // outliner. See the class comment for why that matters here.
        USceneCaptureComponent2D* Component = NewObject<USceneCaptureComponent2D>(
            GetTransientPackage(), NAME_None, RF_Transient);
        if (!Component)
        {
            return;
        }

        // On-demand capture only. Both of these ship TRUE, and either one left on makes the
        // component re-render every frame for as long as it is registered — a permanent
        // per-frame cost bolted onto the editor by a read-only analysis call.
        Component->bCaptureEveryFrame = false;
        Component->bCaptureOnMovement = false;
        // Deliberately NOT bAlwaysPersistRenderingState. Without a persistent view state the
        // capture has no temporal history, which forces FSceneView::SetupAntiAliasingMethod
        // down to AAM_None and leaves no TAA/TSR jitter. That is exactly what this probe
        // wants: two captures of the same world state differ ONLY by the parameter the
        // caller perturbed, with no temporal noise underneath. Turning it on would allocate
        // a view state per component and reintroduce the jitter.
        Component->bAlwaysPersistRenderingState = false;
        Component->ProjectionType = ECameraProjectionMode::Perspective;
        // DM_Low is the lowest detail mode, so IsCulledByDetailMode() can never be true.
        // A detail-mode cull makes CaptureScene() a silent no-op that leaves the render
        // target at its clear colour and still returns normally.
        Component->DetailMode = EDetailMode::DM_Low;
        Component->SetVisibility(true);

        Component->RegisterComponentWithWorld(InWorld);
        if (!Component->IsRegistered())
        {
            return;
        }

        CaptureComponent.Reset(Component);
    }

    FSceneCaptureProbe::~FSceneCaptureProbe()
    {
        if (USceneCaptureComponent2D* Component = CaptureComponent.Get())
        {
            Component->TextureTarget = nullptr;
            if (Component->IsRegistered())
            {
                Component->UnregisterComponent();
            }
        }
        if (UTextureRenderTarget2D* Target = RenderTarget.Get())
        {
            Target->ReleaseResource();
        }
        // Both handles are released here and collected by the next ordinary GC. This path
        // never calls CollectGarbage(): a synchronous collect from inside an RPC is the
        // reentrancy crash recorded as B-python-execute-reentrant-gc-crash.
        CaptureComponent.Reset();
        RenderTarget.Reset();
    }

    bool FSceneCaptureProbe::IsValid() const
    {
        return World.IsValid() && CaptureComponent.IsValid() && CaptureComponent->IsRegistered();
    }

    bool FSceneCaptureProbe::EnsureRenderTarget(int32 Width, int32 Height,
        bool bSingleChannelDepth, bool bEightBitColor,
        FString& OutErrorCode, FString& OutErrorMessage)
    {
        if (RenderTarget.IsValid() && RenderTargetWidth == Width && RenderTargetHeight == Height
            && bRenderTargetIsDepth == bSingleChannelDepth
            && bRenderTargetIsColor8 == bEightBitColor)
        {
            return true;
        }

        if (UTextureRenderTarget2D* Previous = RenderTarget.Get())
        {
            Previous->ReleaseResource();
            RenderTarget.Reset();
        }

        UTextureRenderTarget2D* Target = NewObject<UTextureRenderTarget2D>(
            GetTransientPackage(), NAME_None, RF_Transient);
        if (!Target)
        {
            OutErrorCode = ErrorCodes::ERR_RENDER_TARGET_CREATE_FAILED;
            OutErrorMessage = TEXT("Could not allocate the analysis render target");
            return false;
        }

        // PF_R32_FLOAT for depth: SCS_SceneDepth writes raw linear centimetres with no clamp,
        // so an 8-bit UNORM target (the format every other capture in this plugin uses) would
        // saturate to white past 1 cm and silently return a uniform image. PF_FloatRGBA for
        // the G-buffer sources: half floats carry the channel exactly enough that two renders
        // of an unchanged surface come back bit-identical, which is the whole basis of the
        // comparison built on top of this.
        // UE 5.3's FTextureRenderTargetResource::IsSupportedFormat does not list PF_R32_FLOAT
        // (TextureRenderTarget.cpp:73-90; 5.4 widened the list), and InitCustomFormat check()s it,
        // so asking for it there is a hard assert rather than a soft rejection. PF_FloatRGBA is
        // supported on every supported engine and is still a float format, so depth stays linear
        // and unclamped instead of saturating the way an 8-bit UNORM target would. The cost on 5.3
        // is half-float precision (~11-bit mantissa, finite up to 65504 cm) in the R channel, which
        // is the only channel the depth consumer reads (ZFightingHandler.cpp:492).
        Target->ClearColor = FLinearColor::Black;
        Target->bAutoGenerateMips = false;
        if (bEightBitColor)
        {
            Target->RenderTargetFormat = RTF_RGBA8_SRGB;
            Target->bForceLinearGamma = false;
            Target->SRGB = true;
            Target->InitAutoFormat(static_cast<uint32>(Width), static_cast<uint32>(Height));
        }
        else
        {
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 4, 0)
            const EPixelFormat Format = bSingleChannelDepth ? PF_R32_FLOAT : PF_FloatRGBA;
#else
            const EPixelFormat Format = PF_FloatRGBA;
#endif
            // bForceLinearGamma: these pixels are measurements. An sRGB encode on the way out
            // would apply a non-linear curve to depth centimetres and to normal components.
            Target->InitCustomFormat(Width, Height, Format, /*bInForceLinearGamma=*/true);
        }
        Target->UpdateResourceImmediate(/*bClearRenderTarget=*/true);

        RenderTarget.Reset(Target);
        RenderTargetWidth = Width;
        RenderTargetHeight = Height;
        bRenderTargetIsDepth = bSingleChannelDepth;
        bRenderTargetIsColor8 = bEightBitColor;
        return true;
    }

    bool FSceneCaptureProbe::Capture(const FProbeRequest& Request, TArray<FLinearColor>& OutPixels,
        FString& OutErrorCode, FString& OutErrorMessage)
    {
        OutPixels.Reset();
        OutErrorCode.Reset();
        OutErrorMessage.Reset();

        if (!IsValid())
        {
            OutErrorCode = ErrorCodes::ERR_SCENE_CAPTURE_FAILED;
            OutErrorMessage = TEXT("Scene capture probe is not initialised");
            return false;
        }
        if (Request.Width <= 0 || Request.Height <= 0
            || Request.Width > MaxProbeDimension || Request.Height > MaxProbeDimension)
        {
            OutErrorCode = ErrorCodes::ERR_INVALID_ARGUMENT;
            OutErrorMessage = FString::Printf(TEXT("width and height must be in (0, %d]"), MaxProbeDimension);
            return false;
        }
        if (Request.NearClippingPlane <= 0.0f)
        {
            OutErrorCode = ErrorCodes::ERR_INVALID_ARGUMENT;
            OutErrorMessage = TEXT("nearPlane must be greater than zero (world centimetres)");
            return false;
        }

        const bool bDepth = SourceIsSingleChannelDepth(Request.Source);
        if (!EnsureRenderTarget(Request.Width, Request.Height, bDepth,
            /*bEightBitColor=*/false, OutErrorCode, OutErrorMessage))
        {
            return false;
        }

        USceneCaptureComponent2D* Component = CaptureComponent.Get();
        Component->TextureTarget = RenderTarget.Get();
        Component->CaptureSource = Request.Source;
        Component->FOVAngle = Request.Fov;
        Component->ProjectionType = ECameraProjectionMode::Perspective;
        // The near plane is the whole point of this probe: it is a per-capture override with
        // no global state behind it, unlike r.SetNearClipPlane which sets GNearClippingPlane
        // process-wide and would corrupt whatever a concurrent agent is rendering.
        Component->bOverride_CustomNearClippingPlane = true;
        Component->CustomNearClippingPlane = Request.NearClippingPlane;
        // Both fields are required together: the renderer only reads MaxViewDistanceOverride
        // as a far plane when bFiniteFarPlane is set, and leaves far == near otherwise, which
        // is the infinite-far branch. Clearing both restores that default.
        //
        // bFiniteFarPlane arrived in UE 5.8. On older engines MaxViewDistanceOverride is a
        // draw-distance cull only and there is no way to give a scene capture a finite far
        // plane, so the projection stays on the infinite-far branch there.
        if (Request.FarClippingPlane > Request.NearClippingPlane)
        {
            Component->MaxViewDistanceOverride = Request.FarClippingPlane;
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 8, 0)
            Component->bFiniteFarPlane = true;
#endif
        }
        else
        {
            Component->MaxViewDistanceOverride = -1.0f;
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 8, 0)
            Component->bFiniteFarPlane = false;
#endif
        }
        Component->SetWorldLocationAndRotation(Request.Location, Request.Rotation);

        // Synchronous: CaptureScene sends end-of-frame updates, builds a scene render and
        // executes it before returning. CaptureSceneDeferred would only queue the work, to be
        // drained when a main view family renders — which in a headless or idle editor may
        // never happen, giving a clean success over an unwritten render target.
        Component->CaptureScene();
        ++RenderCount;

        // ReadLinearColorPixels flushes internally, but the explicit flush keeps the
        // draw-then-flush-then-read ordering visible and matches WidgetDesignerCaptureUtil.
        FlushRenderingCommands();

        FTextureRenderTargetResource* Resource = RenderTarget->GameThread_GetRenderTargetResource();
        if (!Resource)
        {
            OutErrorCode = ErrorCodes::ERR_READ_PIXELS_FAILED;
            OutErrorMessage = TEXT("Analysis render target has no render resource");
            return false;
        }

        // RCM_MinMax (the default for this call) means "do not renormalise": the floats come
        // back exactly as the shader wrote them. RCM_UNorm, the default on the FColor
        // ReadPixels overload, would rescale them.
        if (!Resource->ReadLinearColorPixels(OutPixels))
        {
            OutErrorCode = ErrorCodes::ERR_READ_PIXELS_FAILED;
            OutErrorMessage = TEXT("Could not read the analysis render target back to the CPU");
            return false;
        }

        const int64 Expected = static_cast<int64>(Request.Width) * static_cast<int64>(Request.Height);
        if (OutPixels.Num() != Expected)
        {
            OutPixels.Reset();
            OutErrorCode = ErrorCodes::ERR_READ_PIXELS_FAILED;
            OutErrorMessage = FString::Printf(
                TEXT("Analysis readback returned %d pixels, expected %lld"), OutPixels.Num(), Expected);
            return false;
        }

        return true;
    }

    const FEngineShowFlags* FSceneCaptureProbe::GetShowFlags() const
    {
        const USceneCaptureComponent2D* Component = CaptureComponent.Get();
        return Component ? &Component->ShowFlags : nullptr;
    }

    bool FSceneCaptureProbe::CaptureColor(const FColorCaptureRequest& Request,
        TArray<FColor>& OutPixels, FColorCaptureMetadata& OutMetadata,
        FString& OutErrorCode, FString& OutErrorMessage)
    {
        OutPixels.Reset();
        OutMetadata = FColorCaptureMetadata();
        OutErrorCode.Reset();
        OutErrorMessage.Reset();

        if (!IsValid())
        {
            OutErrorCode = ErrorCodes::ERR_SCENE_CAPTURE_FAILED;
            OutErrorMessage = TEXT("Scene capture is not initialised");
            return false;
        }
        if (Request.Width <= 0 || Request.Height <= 0
            || Request.Width > MaxProbeDimension || Request.Height > MaxProbeDimension)
        {
            OutErrorCode = ErrorCodes::ERR_INVALID_ARGUMENT;
            OutErrorMessage = FString::Printf(
                TEXT("width and height must be in (0, %d]"), MaxProbeDimension);
            return false;
        }
        if (Request.Fov <= 0.0f || (Request.bOrthographic && Request.OrthoWidth <= 0.0f))
        {
            OutErrorCode = ErrorCodes::ERR_INVALID_ARGUMENT;
            OutErrorMessage = Request.bOrthographic
                ? TEXT("orthoWidth must be greater than zero")
                : TEXT("fov must be greater than zero");
            return false;
        }
        if (!FMath::IsFinite(Request.NearClippingPlane)
            || Request.NearClippingPlane <= 0.0f)
        {
            OutErrorCode = ErrorCodes::ERR_INVALID_ARGUMENT;
            OutErrorMessage = TEXT("near clipping plane must be finite and greater than zero");
            return false;
        }
        if (!EnsureRenderTarget(Request.Width, Request.Height,
            /*bSingleChannelDepth=*/false, /*bEightBitColor=*/true,
            OutErrorCode, OutErrorMessage))
        {
            return false;
        }

        USceneCaptureComponent2D* Component = CaptureComponent.Get();
        Component->TextureTarget = RenderTarget.Get();
        Component->CaptureSource = SCS_FinalColorLDR;
        Component->PrimitiveRenderMode =
            ESceneCapturePrimitiveRenderMode::PRM_RenderScenePrimitives;
        // No view state and no AA show flag: this path reports AAM_None and produces one
        // deterministic spatial sample rather than carrying temporal history between shots.
        Component->bAlwaysPersistRenderingState = false;
        Component->ProjectionType = Request.bOrthographic
            ? ECameraProjectionMode::Orthographic : ECameraProjectionMode::Perspective;
        Component->FOVAngle = Request.Fov;
        Component->OrthoWidth = Request.OrthoWidth;
        Component->bOverride_CustomNearClippingPlane = true;
        Component->CustomNearClippingPlane = Request.NearClippingPlane;
        Component->MaxViewDistanceOverride = -1.0f;
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 8, 0)
        Component->bFiniteFarPlane = false;
#endif

        Component->ShowFlags = PinWrightOrthoTiles::MakeBaseCaptureShowFlags();
        PinWrightRenderCapture::FViewModePin ViewModePin;
        ViewModePin.bRequested = Request.bViewModeRequested;
        ViewModePin.ViewMode = Request.ViewMode;
        ViewModePin.Key = Request.ViewModeKey;
        ViewModePin.DistinguishingShowFlags = Request.DistinguishingShowFlags;
        const PinWrightOrthoTiles::FViewModePlan ViewMode =
            PinWrightOrthoTiles::ApplyViewModeToCaptureShowFlags(
                ViewModePin, Component->ShowFlags);
        Component->ShowFlags.SetAntiAliasing(false);
        OutMetadata.bViewModeApplied = Request.bViewModeRequested && ViewMode.bApplied;
        OutMetadata.AppliedViewModeKey = Request.bViewModeRequested
            ? ViewMode.AppliedKey : ViewMode.DerivedKey;
        OutMetadata.ViewModeShowFlagMismatches = ViewMode.ShowFlagMismatches;

        Component->PostProcessSettings = FPostProcessSettings();
        Component->PostProcessBlendWeight = 0.0f;
        if (Request.bFixedExposure)
        {
            PinWrightScreenshotUtils::ApplyFixedEv100ToPostProcessSettings(
                Component->PostProcessSettings, Request.ExposureEv100);
            Component->PostProcessBlendWeight = 1.0f;
            const FPostProcessSettings& Settings = Component->PostProcessSettings;
            OutMetadata.ExposureEv100Applied =
                PinWrightOrthoTiles::PhysicalCameraEv100(
                    Settings.DepthOfFieldFstop,
                    Settings.CameraShutterSpeed,
                    Settings.CameraISO);
            OutMetadata.bExposurePinned = FMath::IsNearlyEqual(
                OutMetadata.ExposureEv100Applied, Request.ExposureEv100, 1.0e-3f)
                && Component->ShowFlags.Lighting && Component->ShowFlags.PostProcessing;
        }

        Component->SetWorldLocationAndRotation(Request.Location, Request.Rotation);
        Component->CaptureScene();
        ++RenderCount;

        FTextureRenderTargetResource* Resource =
            RenderTarget->GameThread_GetRenderTargetResource();
        if (!Resource || !Resource->ReadPixels(OutPixels))
        {
            OutPixels.Reset();
            OutErrorCode = ErrorCodes::ERR_READ_PIXELS_FAILED;
            OutErrorMessage = TEXT("Could not read the scene-color capture back to the CPU");
            return false;
        }
        const int64 Expected = static_cast<int64>(Request.Width)
            * static_cast<int64>(Request.Height);
        if (OutPixels.Num() != Expected)
        {
            OutPixels.Reset();
            OutErrorCode = ErrorCodes::ERR_READ_PIXELS_FAILED;
            OutErrorMessage = FString::Printf(
                TEXT("Scene-color readback returned %d pixels, expected %lld"),
                OutPixels.Num(), static_cast<long long>(Expected));
            return false;
        }
        PinWrightScreenshotUtils::ForceOpaqueAlpha(OutPixels);
        return true;
    }

    FVector PixelToCameraRay(const FRotator& CameraRotation, float Fov,
        int32 Width, int32 Height, double PixelX, double PixelY)
    {
        if (Width <= 0 || Height <= 0)
        {
            return CameraRotation.Vector();
        }

        const FRotationMatrix RotationMatrix(CameraRotation);
        const FVector Forward = RotationMatrix.GetScaledAxis(EAxis::X);
        const FVector Right = RotationMatrix.GetScaledAxis(EAxis::Y);
        const FVector Up = RotationMatrix.GetScaledAxis(EAxis::Z);

        // BuildProjectionMatrix (Renderer/Private/SceneCaptureRendering.cpp) feeds
        // FReversedZPerspectiveMatrix an X multiplier of 1 and a Y multiplier of Width/Height,
        // so FOVAngle is the full HORIZONTAL field of view and the vertical half-extent is
        // scaled by Height/Width.
        const double TanHalfFov = FMath::Tan(FMath::DegreesToRadians(static_cast<double>(Fov) * 0.5));
        const double NdcX = (2.0 * (PixelX + 0.5) / static_cast<double>(Width)) - 1.0;
        const double NdcY = 1.0 - (2.0 * (PixelY + 0.5) / static_cast<double>(Height));

        return Forward
            + Right * (NdcX * TanHalfFov)
            + Up * (NdcY * TanHalfFov * static_cast<double>(Height) / static_cast<double>(Width));
    }
}
