// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Handlers/Asset/ThumbnailFrameEvidence.h"
#include "Handlers/Render/FlatRegionStats.h"
#include "Handlers/Render/PreviewViewportCaptureUtils.h"
#include "Templates/Function.h"

class FJsonValue;
class UMeshComponent;

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

    // Nanite page residency for one shot (#15). SettleFrame's redraws all run inside one engine
    // frame, and the Nanite streaming manager reads back GPU page requests and installs streamed
    // pages only once per render-thread frame number (Engine NaniteStreamingManager.cpp:2312), so
    // two in-tick draws of a cold Nanite mesh agree at its coarse root clusters. For a mesh that
    // renders through Nanite, each shot is therefore redrawn across advanced frame numbers until
    // NaniteStableFrames consecutive frames agree, bounded by NaniteMaxPumpedFrames.
    struct FNaniteResidencyReport
    {
        // Frames were pumped for this shot. False with SkipReason when the mesh does not draw
        // through Nanite here, so there are no pages to wait for.
        bool bWaited = false;
        FString SkipReason;
        int32 PumpedFrames = 0;
        // Some pumped frame differed from the one before it: the in-tick settle alone would have
        // returned the earlier, coarser frame.
        bool bChangedAcrossFrames = false;
        bool bSettled = false;
        // The shot stopped unsettled because the call's pump budget (NaniteSessionPumpBudget) ran
        // out, not because it hit NaniteMaxPumpedFrames itself.
        bool bBudgetExhausted = false;
        // Pixels changed between the last two pumped frames.
        double ChangedPixelFraction = 0.0;
    };

    // ponytail: frame-count heuristics, not a residency query - the engine exposes no "all pages
    // this view wants are resident" API (NaniteStreamingManager.h keeps the pending-page state
    // private). One request round takes about 3 pumps: requests drawn at frame K are copied back
    // at K+1, locked at K+2 (NaniteReadbackManager.cpp:94-113) and the pages they name installed
    // at K+3 (DetermineReadyOrSkippedPages). 8 stable frames cover two rounds; raise them if a
    // slow DDC still settles early.
    constexpr int32 NaniteStableFrames = 8;
    constexpr int32 NaniteMaxPumpedFrames = 120;
    constexpr float NanitePumpSleepSeconds = 0.01f;

    // The whole call's pump budget, shared by its shots: pumped draws may add at most
    // NanitePumpPixelBudget rendered pixels (the size of render.capture_mesh's own request budget),
    // floored at 2 * NaniteStableFrames so one large shot can still settle, and capped at
    // NaniteSessionMaxPumpedFrames so the worst case stays far below the proxy call timeout.
    constexpr int64 NanitePumpPixelBudget = 64ll * 1024ll * 1024ll;
    constexpr int32 NaniteSessionMaxPumpedFrames = 240;
    int32 NaniteSessionPumpBudget(int64 FramePixels);

    // InOutPixels holds the in-tick frame. AdvanceAndRedraw advances the frame number and draws
    // the same request again; the newest frame always replaces InOutPixels. Stops once
    // StableFrames consecutive redraws agree with their predecessor within the SettleFrame
    // tolerance, or after MaxFrames redraws (bSettled false). False only when a redraw fails.
    bool SettleAcrossFrames(TFunctionRef<bool(TArray<FColor>&)> AdvanceAndRedraw,
        TArray<FColor>& InOutPixels, int32 StableFrames, int32 MaxFrames,
        FNaniteResidencyReport& OutReport);

    // Starts a new render-thread frame number without ending the engine frame, the way the
    // engine's own high-resolution screenshot run-up does (UnrealClient.cpp:1603-1611).
    void AdvanceRenderFrameNumber();

    TSharedPtr<FJsonObject> MakeNaniteResidencyObject(const FNaniteResidencyReport& Report);

    // Empty unless the shot pumped frames and stopped unsettled.
    FString MakeNaniteWarning(const FNaniteResidencyReport& Report);

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
        FNaniteResidencyReport Nanite;
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

        // The session's transient mesh component, for probing what the last draw rendered.
        UMeshComponent* GetMeshComponent() const;

    private:
        struct FImpl;
        explicit FMeshCaptureSession(TUniquePtr<FImpl>&& InImpl);
        TUniquePtr<FImpl> Impl;
    };

    // `readiness` (what the session waited for before its first draw, plus this shot's
    // `naniteResidency`) and, on a shot that measured it, `frameSettled` /
    // `settleChangedPixelFraction` plus a `frameWarning` when the frame never settled or the Nanite
    // wait hit its bound. `redrawRetries` itself is published by the shared capture fields.
    void AddMeshFrameEvidenceFields(const FMeshCaptureOutput& Capture,
        const TSharedPtr<FJsonObject>& Result);

    // Multi-shot calls: each shots[i] gets its own `naniteResidency` and, when its Nanite wait
    // stopped unsettled, its own `frameWarning`; the top level gets `naniteUnsettledShots` and a
    // `frameWarning` naming them, so a later shot's bound is as loud as shot 0's.
    void AddNaniteShotFields(TConstArrayView<FMeshCaptureOutput> Captures,
        TConstArrayView<TSharedPtr<FJsonValue>> Shots, const TSharedPtr<FJsonObject>& Result);

    bool CaptureMeshToPng(const FMeshCaptureRequest& Request, FMeshCaptureOutput& OutCapture,
        FString& OutErrCode, FString& OutErrMsg);

    // Fills the `viewport` block's view-mode, exposure and render fields for one scene-capture
    // probe draw. Shared with render.capture_actor_preview so both verbs report the same block.
    void ConfigureMeshPreviewOutput(const PinWrightRenderCapture::FViewportCaptureRequest& Request,
        const PinWrightSceneCaptureProbe::FColorCaptureRequest& ColorRequest,
        const PinWrightSceneCaptureProbe::FColorCaptureMetadata& Metadata,
        PinWrightRenderCapture::FViewportCaptureOutput& Out);
}
