// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Handlers/Render/PreviewViewportCaptureUtils.h"

// Stateless screen<->world view/projection helper for an editor viewport.
//
// WHICH VIEWPORT. Every entry point takes an OPTIONAL FViewProjectionTarget. Omit it (the
// default) and the view is rebuilt from the active Level Editor viewport, exactly as this file
// has always done. Supply one and the view is rebuilt from THAT client instead - which is what
// lets an overlay be projected over an asset-editor preview frame. The target is not a
// convenience: an overlay projected through a DIFFERENT viewport than the one that rendered the
// pixels lands on coordinates that frame never had, while still producing a plausible PNG, so the
// capture and the projection must be handed the same client by construction.
//
// The plugin stores NO persistent FSceneView: the engine builds one transiently inside
// SceneViewport->Draw() and discards it. To deproject/project a pixel identically to a
// render.capture_* call, this util rebuilds the SAME view via FEditorViewportClient::CalcSceneView
// after applying the caller's pose through PinWrightRenderCapture::ApplyCaptureCamera - the very
// function the capture path calls, including the orthographic rotation -> ELevelViewportType
// mapping and the world-centimetre orthoWidth -> ortho-zoom conversion - and the same
// SetFixedViewportSize(width, height). Callers therefore pass the SAME
// PinWrightRenderCapture::FViewportCaptureRequest they captured with, guaranteeing the rebuilt
// view is pose-coherent with the pixels on disk. An orthographic pose that does not look along a
// world axis is rejected here exactly as the capture would have rejected it.
//
// Both functions temporarily mutate the TARGET viewport's camera + fixed size, build the view,
// and restore the originals on scope exit (RAII), matching CaptureEditorViewportToPng's save/apply/
// restore contract. Neither renders pixels, so no RHI readback and no realtime override are needed.
//
// Foundation code consumed by spatial.raycast_screen (deproject) and render.capture_annotated
// (project AABB corners and actor centroids to overlay pixels).
//
// COST MODEL - read before adding a caller. Rebuilding the view is the expensive part: it saves
// the live viewport camera, resizes it, runs CalcSceneView and restores everything. Projecting a
// point against an already-built view is one matrix transform. Anything projecting more than a
// couple of points MUST therefore open an FViewProjectionSession once and reuse it; calling the
// one-shot ProjectWorldToScreen in a loop is O(n) viewport thrash on the game thread. Nesting two
// sessions ON THE SAME VIEWPORT is also a correctness bug, not just a slow one: the inner one
// would snapshot the already-mutated camera as "the original" and restore the viewport to that.
// Two sessions against DIFFERENT clients do not interact - each saves and restores only its own -
// but the level-viewport default resolves to one shared client, so two defaulted sessions do.
namespace PinWrightViewProjection
{
    namespace Detail { struct FScopedRebuiltView; }

    // WHICH viewport a view is rebuilt from.
    //
    // Default-constructed (Client null) means "the active Level Editor viewport", resolved
    // internally through FLevelEditorModule::GetFirstActiveViewport() - the behaviour every caller
    // had before this struct existed, and still the behaviour of every call that omits a target.
    //
    // BOTH FIELDS OR NEITHER. The client supplies the camera and the scene; the scene viewport
    // supplies the pixel rect CalcSceneView reads the view rect from, and it is what
    // SetFixedViewportSize is applied to. A client without its scene viewport cannot produce a
    // view of the requested dimensions, so it is refused with PREVIEW_VIEWPORT_NOT_FOUND rather
    // than silently rebuilt at whatever size the widget happens to be - which would put every
    // projected pixel on a different grid from the captured PNG.
    //
    // The two members are exactly the pair FResolvedSubject hands back
    // (CaptureSubject.h, ViewportClient + SceneViewport) and exactly the pair
    // CaptureEditorViewportToPng takes, so a verb passes the SAME two values to the capture and to
    // the projection and cannot get them out of step.
    struct FViewProjectionTarget
    {
        FEditorViewportClient* Client = nullptr;
        TSharedPtr<FSceneViewport> SceneViewport;

        FViewProjectionTarget() = default;
        FViewProjectionTarget(FEditorViewportClient& InClient,
            const TSharedPtr<FSceneViewport>& InSceneViewport)
            : Client(&InClient), SceneViewport(InSceneViewport)
        {
        }

        // A target was supplied at all. False means "resolve the active Level Editor viewport".
        bool IsExplicit() const { return Client != nullptr; }
    };

    // One rebuilt, pose-coherent FSceneView held open across many projections.
    //
    // Construct with the SAME FViewportCaptureRequest the capture used; the view is built once in
    // the constructor (through PinWrightRenderCapture::ApplyCaptureCamera, including the
    // orthographic rotation -> ELevelViewportType mapping) and the viewport is restored in the
    // destructor, matching CaptureEditorViewportToPng's save/apply/restore contract. Renders no
    // pixels: no RHI readback, no realtime override.
    //
    // IsValid() is false when there is no editor / editor world / active Level Editor viewport (or,
    // with an explicit target, when that client has no scene viewport), or when the pose is a
    // tilted orthographic one; GetError() then carries the typed reason. A
    // caller that can degrade (an overlay painter) may simply skip drawing; a caller that cannot
    // must surface GetError() as it would have surfaced ProjectWorldToScreen's OutErr.
    //
    // Game-thread only, non-copyable, and NOT re-entrant - see the cost-model note above.
    class FViewProjectionSession
    {
    public:
        explicit FViewProjectionSession(const PinWrightRenderCapture::FViewportCaptureRequest& View);
        // Rebuild the view from THIS client instead of the active Level Editor viewport. Pass the
        // same client and scene viewport the capture rendered through; see FViewProjectionTarget.
        FViewProjectionSession(const PinWrightRenderCapture::FViewportCaptureRequest& View,
            const FViewProjectionTarget& Target);
        ~FViewProjectionSession();

        FViewProjectionSession(const FViewProjectionSession&) = delete;
        FViewProjectionSession& operator=(const FViewProjectionSession&) = delete;

        bool IsValid() const;
        const FString& GetError() const;

        // Project one world point against the view this session already built. Semantics are
        // identical to ProjectWorldToScreen's (TOP-LEFT pixel origin; a behind-camera point still
        // yields a mirrored pixel and sets OutBehindCamera). Returns false only when the session
        // itself is invalid, in which case the out-params are left at their zero/false defaults.
        bool Project(const FVector& WorldPoint, float& OutPixelX, float& OutPixelY,
            bool& OutBehindCamera) const;

    private:
        TUniquePtr<Detail::FScopedRebuiltView> Scoped;
    };

    // Deproject a viewport pixel to a world-space ray.
    //
    // View       - the SAME pose/dimensions a capture used (location, rotation, projectionMode,
    //              fov/orthoWidth, width, height). Reused directly for capture pose-coherence.
    // PixelX/Y   - pixel coordinates, TOP-LEFT origin: (0,0) is the top-left corner, (Width,Height)
    //              the bottom-right. The center pixel (Width/2, Height/2) maps to camera forward.
    // OutWorldOrigin - ray start on the near plane (perspective) or on the ortho near face.
    // OutWorldDir    - unit world-space ray direction (points into the scene, away from the camera).
    // OutErr     - typed error string on failure (e.g. "NO_ACTIVE_LEVEL_VIEWPORT: ...").
    // Target     - which viewport to rebuild the view from. Omit for the active Level Editor
    //              viewport (unchanged behaviour); see FViewProjectionTarget.
    //
    // Returns false (with OutErr set, no crash) when there is no editor / no editor world / no active
    // Level Editor viewport. Handles both perspective and orthographic projection modes.
    bool DeprojectScreenToWorld(
        const PinWrightRenderCapture::FViewportCaptureRequest& View,
        float PixelX,
        float PixelY,
        FVector& OutWorldOrigin,
        FVector& OutWorldDir,
        FString& OutErr,
        const FViewProjectionTarget& Target = FViewProjectionTarget());

    // Project a world-space point to a viewport pixel (inverse of DeprojectScreenToWorld).
    //
    // View            - the SAME pose/dimensions a capture used (see DeprojectScreenToWorld).
    // WorldPoint      - world-space point to project (e.g. an AABB corner for an overlay box).
    // OutPixelX/Y     - pixel coordinates, TOP-LEFT origin. Always written when the call returns true,
    //                   even for points behind the camera (mirrored via |W|) so AABB overlays can clip;
    //                   consult OutBehindCamera before trusting an off-screen result.
    // OutBehindCamera - true when WorldPoint is behind the camera plane (projected pixel is unreliable).
    // OutErr          - typed error string on failure.
    // Target          - which viewport to rebuild the view from. Omit for the active Level Editor
    //                   viewport (unchanged behaviour); see FViewProjectionTarget.
    //
    // Returns false (with OutErr set, no crash) when the view cannot be built (no viewport / world).
    // A successful build with a behind-camera point still returns true with OutBehindCamera = true.
    bool ProjectWorldToScreen(
        const PinWrightRenderCapture::FViewportCaptureRequest& View,
        const FVector& WorldPoint,
        float& OutPixelX,
        float& OutPixelY,
        bool& OutBehindCamera,
        FString& OutErr,
        const FViewProjectionTarget& Target = FViewProjectionTarget());
}
