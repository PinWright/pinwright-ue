// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Handlers/Asset/ThumbnailFrameEvidence.h"
#include "Handlers/Render/FlatRegionStats.h"
#include "Handlers/Render/PreviewViewportCaptureUtils.h"
#include "Templates/Function.h"

namespace PinWrightSceneCaptureProbe
{
    struct FColorCaptureRequest;
    struct FColorCaptureMetadata;
}

namespace PinWrightMeshPreviewCapture
{
    // Production boundary shared by the RPC and the GPU-gated automation test. Each call owns a
    // new preview world, rig, mesh component, scene-capture component and render target; no asset
    // editor, active world, UAssetViewerSettings profile or viewport state is consulted.
    struct FMeshCaptureRequest
    {
        FString AssetPath;
        PinWrightRenderCapture::FViewportCaptureRequest Capture;
        bool bUseOrbitPose = true;
        bool bAutoFrameOrthoWidth = true;
        float OrbitAzimuth = 0.0f;
        float OrbitElevation = 20.0f;
        float Padding = 1.25f;
        float SubjectScale = 1.0f;
        bool bMeasureCoverage = true;
        bool bWriteFile = true;
    };

    // Whether the first shot of a session was drawn again with identical inputs until two
    // consecutive frames agreed (B-capture-mesh-cold-first-frame-no-readiness-wait). Only the
    // session's first shot measures it; later shots reuse the warmed scene.
    struct FFrameSettleReport
    {
        bool bMeasured = false;
        bool bSettled = false;
        // Pixels changed (any channel over SettleChannelThreshold) between the last two draws.
        double ChangedPixelFraction = 0.0;
    };

    constexpr int32 SettleChannelThreshold = 8;
    // ponytail: fixed tolerance for GPU noise between two warm draws; make it a param if a
    // legitimately noisy subject keeps reporting settled:false.
    constexpr double SettledChangedPixelFraction = 0.001;
    constexpr int32 MaxSettleRedrawRetries = 1;

    // InOutPixels holds the first draw. Redraw draws the same request again; the newest frame
    // always replaces InOutPixels. Stops when two consecutive frames agree within the tolerance
    // above, or after MaxRetries redraws past the first comparison draw. OutRetries counts those
    // redraws (published as `redrawRetries`). False only when Redraw fails.
    bool SettleFrame(TFunctionRef<bool(TArray<FColor>&)> Redraw, TArray<FColor>& InOutPixels,
        int32 MaxRetries, int32& OutRetries, FFrameSettleReport& OutReport);

    struct FMeshCaptureOutput
    {
        FString AssetPath;
        FString AssetClass;
        PinWrightRenderCapture::FViewportCaptureRequest ResolvedRequest;
        PinWrightRenderCapture::FViewportCaptureOutput Capture;
        PinWrightFlatRegion::FFlatRegionStats FlatRegion;
        TOptional<double> SubjectCoverage;
        TArray<uint8> PngData;
        PinWrightThumbnail::FThumbnailReadinessReport Readiness;
        FFrameSettleReport Settle;
    };

    // One RPC-scoped scene. Multi-shot callers reuse the loaded asset, mesh component, rig,
    // capture component and render target instead of rebuilding them for every camera pose.
    class FMeshCaptureSession
    {
    public:
        static TUniquePtr<FMeshCaptureSession> Create(const FString& AssetPath,
            const PinWrightPreviewSceneRig::FPreviewSceneRigPin& RigPin,
            FString& OutErrCode, FString& OutErrMsg, float SubjectScale = 1.0f);
        ~FMeshCaptureSession();

        bool Capture(const FMeshCaptureRequest& Request, FMeshCaptureOutput& OutCapture,
            FString& OutErrCode, FString& OutErrMsg);

    private:
        struct FImpl;
        explicit FMeshCaptureSession(TUniquePtr<FImpl>&& InImpl);
        TUniquePtr<FImpl> Impl;
    };

    // `readiness` (what the session waited for before its first draw) and, on a shot that measured
    // it, `frameSettled` / `settleChangedPixelFraction` plus a `frameWarning` when the frame never
    // settled. `redrawRetries` itself is published by the shared capture fields.
    void AddMeshFrameEvidenceFields(const FMeshCaptureOutput& Capture,
        const TSharedPtr<FJsonObject>& Result);

    bool CaptureMeshToPng(const FMeshCaptureRequest& Request, FMeshCaptureOutput& OutCapture,
        FString& OutErrCode, FString& OutErrMsg);

    // Fills the `viewport` block's view-mode, exposure and render fields for one scene-capture
    // probe draw. Shared with render.capture_actor_preview so both verbs report the same block.
    void ConfigureMeshPreviewOutput(const PinWrightRenderCapture::FViewportCaptureRequest& Request,
        const PinWrightSceneCaptureProbe::FColorCaptureRequest& ColorRequest,
        const PinWrightSceneCaptureProbe::FColorCaptureMetadata& Metadata,
        PinWrightRenderCapture::FViewportCaptureOutput& Out);
}
