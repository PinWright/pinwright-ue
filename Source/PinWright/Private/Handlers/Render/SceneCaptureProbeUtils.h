// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Engine/EngineBaseTypes.h" // EViewModeIndex
#include "Engine/EngineTypes.h" // ESceneCaptureSource
#include "Handlers/Render/CaptureDefaults.h"
#include "UObject/StrongObjectPtr.h"

class UWorld;
class USceneCaptureComponent2D;
class UTextureRenderTarget2D;
struct FEngineShowFlags;

// Offscreen scene-capture probe: renders the supplied world through a transient
// USceneCaptureComponent2D into a transient render target and reads the result back on the
// CPU as float pixels.
//
// Why this exists alongside PreviewViewportCaptureUtils. Viewport-backed capture paths in this
// plugin drives the real Level Editor viewport and reads its back buffer
// (FSceneViewport::ReadPixels -> TArray<FColor>). That path can only produce an 8-bit lit
// image, it needs a live Slate viewport, and it moves the user's camera. A probe needs
// none of that and needs things that path cannot give:
//
//   - non-colour capture sources (scene depth as float centimetres, G-buffer base colour,
//     G-buffer world normal),
//   - float render targets (PF_R32_FLOAT / PF_FloatRGBA) instead of BGRA8,
//   - a caller-chosen NEAR CLIPPING PLANE, which is the perturbation lever
//     render.detect_z_fighting is built on,
//   - no viewport, so it works with no Level Editor window open.
//
// The float Capture path is deliberately measurement-only: routing depth or normals through PNG
// colour helpers would destroy them. CaptureColor is the separate picture-producing boundary; it
// uses an 8-bit target, final-colour source, opaque alpha and the same view-mode/exposure helpers as
// the other scene-capture route.
namespace PinWrightSceneCaptureProbe
{
    // Ceiling on either analysis dimension. Matches PreviewViewportCaptureUtils'
    // MaxCaptureDimension so one capture-size rule covers both renderers; a probe at the
    // ceiling allocates 16384*16384*16 bytes of render target, so callers are expected to
    // stay far below it.
    constexpr int32 MaxProbeDimension = 16384;

    // Engine default near clipping plane in centimetres (GNearClippingPlane's shipped
    // value). The probe always sets the near plane EXPLICITLY rather than inheriting the
    // global, because the global is process-wide, is writable at runtime by
    // r.SetNearClipPlane, and would make two probes taken minutes apart incomparable.
    constexpr float DefaultNearClippingPlane = 10.0f;

    struct FProbeRequest
    {
        FVector Location = FVector::ZeroVector;
        FRotator Rotation = FRotator::ZeroRotator;
        // Full HORIZONTAL field of view in degrees, matching the fov parameter every other
        // capture verb takes. USceneCaptureComponent2D::FOVAngle uses the same convention.
        float Fov = 50.0f;
        int32 Width = PinWrightRenderCapture::DefaultCaptureEdge;
        int32 Height = PinWrightRenderCapture::DefaultCaptureEdge;
        // Near clipping plane in centimetres. Always applied via
        // bOverride_CustomNearClippingPlane; see DefaultNearClippingPlane.
        float NearClippingPlane = DefaultNearClippingPlane;
        // Far clipping plane in centimetres, or <= 0 for the engine's default INFINITE far
        // plane. A scene capture leaves far equal to near unless MaxViewDistanceOverride and
        // bFiniteFarPlane are both set, and equal near/far selects FReversedZPerspectiveMatrix's
        // MinZ==MaxZ branch, i.e. an infinite far plane where DeviceZ is exactly Near/W.
        //
        // Setting a finite far plane changes the depth encoding (both M[2][2] and M[3][2])
        // while leaving FViewMatrices::ComputeNearPlane's result unchanged, which matters
        // because that value is what reaches View.NearPlane and, through it, Nanite's
        // per-cluster LOD error metric. It is therefore the perturbation to reach for when a
        // near-plane perturbation is suspected of moving Nanite LOD rather than depth
        // comparisons.
        //
        // Geometry beyond this distance is clipped, and the value also caps the capture's
        // view distance, so it must sit beyond everything in frame.
        float FarClippingPlane = 0.0f;
        // Which buffer to read out. SCS_SceneDepth yields linear depth in centimetres in
        // the R channel; the G-buffer sources yield their channel in RGB.
        ESceneCaptureSource Source = SCS_BaseColor;
    };

    // Picture-producing counterpart to FProbeRequest. It uses the same ownerless component and
    // per-instance render target, but reads opaque BGRA8 final colour suitable for image stats and
    // PNG encoding. Every field is local to this capture object; no viewport or world actor is
    // changed.
    struct FColorCaptureRequest
    {
        FVector Location = FVector::ZeroVector;
        FRotator Rotation = FRotator::ZeroRotator;
        float Fov = 50.0f;
        float OrthoWidth = 2000.0f;
        float NearClippingPlane = DefaultNearClippingPlane;
        int32 Width = PinWrightRenderCapture::DefaultCaptureEdge;
        int32 Height = PinWrightRenderCapture::DefaultCaptureEdge;
        bool bOrthographic = false;
        bool bFixedExposure = false;
        float ExposureEv100 = 0.0f;
        bool bViewModeRequested = false;
        EViewModeIndex ViewMode = VMI_Lit;
        FString ViewModeKey;
        TArray<FString> DistinguishingShowFlags;
    };

    struct FColorCaptureMetadata
    {
        bool bViewModeApplied = false;
        FString AppliedViewModeKey;
        TArray<FString> ViewModeShowFlagMismatches;
        bool bExposurePinned = false;
        float ExposureEv100Applied = 0.0f;
    };

    // True when Source needs a single-channel float target (scene depth) rather than an
    // RGBA float one. Exposed so a caller can reason about the readback layout.
    bool SourceIsSingleChannelDepth(ESceneCaptureSource Source);

    // Owns the transient capture component and render target for the lifetime of one RPC.
    //
    // Construct ONCE per request and reuse across every capture in that request. The
    // component is created ownerless and registered directly into the world with
    // RegisterComponentWithWorld rather than being hung off a spawned actor: an actor spawn
    // touches the level's actor array, shows up in the outliner, interacts with World
    // Partition, and marks the level package dirty, all of which a read-only analysis verb
    // must not do to a level another agent may be editing. An ownerless registered
    // component is the same mechanism FPreviewScene::AddComponent uses and touches none of
    // it.
    class FSceneCaptureProbe
    {
    public:
        explicit FSceneCaptureProbe(UWorld* InWorld);
        ~FSceneCaptureProbe();

        FSceneCaptureProbe(const FSceneCaptureProbe&) = delete;
        FSceneCaptureProbe& operator=(const FSceneCaptureProbe&) = delete;

        bool IsValid() const;

        // Render one frame and read it back. OutPixels is resized to Width*Height, row-major
        // from the TOP-LEFT pixel, matching every other bitmap in this plugin.
        //
        // For SCS_SceneDepth, OutPixels[i].R is LINEAR depth in centimetres measured along
        // the view forward axis (clip-space W), not radial distance from the camera, and is
        // unclamped: pixels with no geometry carry the projection's far value, which for the
        // infinite-far reversed-Z projection a scene capture builds is an enormous number
        // rather than zero.
        //
        // Synchronous: enqueues the render, flushes the rendering thread, then reads back.
        // Game thread only.
        bool Capture(const FProbeRequest& Request, TArray<FLinearColor>& OutPixels,
            FString& OutErrorCode, FString& OutErrorMessage);

        bool CaptureColor(const FColorCaptureRequest& Request, TArray<FColor>& OutPixels,
            FColorCaptureMetadata& OutMetadata, FString& OutErrorCode, FString& OutErrorMessage);

        const FEngineShowFlags* GetShowFlags() const;

        // Number of scene renders issued through this probe so far. Reported by callers so
        // the cost of an analysis is visible in its own response.
        int32 GetRenderCount() const { return RenderCount; }

    private:
        bool EnsureRenderTarget(int32 Width, int32 Height, bool bSingleChannelDepth,
            bool bEightBitColor,
            FString& OutErrorCode, FString& OutErrorMessage);

        TWeakObjectPtr<UWorld> World;
        TStrongObjectPtr<USceneCaptureComponent2D> CaptureComponent;
        TStrongObjectPtr<UTextureRenderTarget2D> RenderTarget;
        int32 RenderTargetWidth = 0;
        int32 RenderTargetHeight = 0;
        bool bRenderTargetIsDepth = false;
        bool bRenderTargetIsColor8 = false;
        int32 RenderCount = 0;
    };

    // Unit direction from the camera through the centre of pixel (PixelX, PixelY), for a
    // perspective probe of the given size and FOV. Screen origin is TOP-LEFT.
    //
    // The returned vector is NOT normalised: its component along the camera forward axis is
    // exactly 1, so multiplying it by a depth read out of an SCS_SceneDepth probe gives the
    // world position of that pixel's surface directly. That works because the depth a scene
    // capture writes is clip-space W (distance along forward), so scaling a
    // forward-component-1 ray by it lands on the surface. Normalising first and multiplying
    // by depth would place the point short of the surface everywhere except the exact centre
    // of the frame.
    FVector PixelToCameraRay(const FRotator& CameraRotation, float Fov,
        int32 Width, int32 Height, double PixelX, double PixelY);
}
