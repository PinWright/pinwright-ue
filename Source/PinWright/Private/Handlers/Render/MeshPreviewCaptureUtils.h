// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Handlers/Render/FlatRegionStats.h"
#include "Handlers/Render/PreviewViewportCaptureUtils.h"

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

    struct FMeshCaptureOutput
    {
        FString AssetPath;
        FString AssetClass;
        PinWrightRenderCapture::FViewportCaptureRequest ResolvedRequest;
        PinWrightRenderCapture::FViewportCaptureOutput Capture;
        PinWrightFlatRegion::FFlatRegionStats FlatRegion;
        TOptional<double> SubjectCoverage;
        TArray<uint8> PngData;
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

    bool CaptureMeshToPng(const FMeshCaptureRequest& Request, FMeshCaptureOutput& OutCapture,
        FString& OutErrCode, FString& OutErrMsg);
}
