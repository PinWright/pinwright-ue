// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"

struct FPostProcessSettings;
class SWidget;

namespace PinWrightScreenshotUtils
{
    constexpr int32 MaxGameViewportCaptureDimension = 16384;
    constexpr int64 MaxGameViewportCapturePixels = 64ll * 1024ll * 1024ll;

    struct FGameViewportCaptureOptions
    {
        // Zero means the live viewport size. A non-zero size requests the exact-size
        // scene-render + off-screen Slate/UMG composite path.
        FIntPoint OutputSize = FIntPoint::ZeroValue;
        bool bPinExposure = false;
        float ExposureEv100 = 0.0f;
    };

    struct FGameViewportCaptureMetadata
    {
        FIntPoint OriginalViewportSize = FIntPoint::ZeroValue;
        bool bViewportWasFixed = false;
        bool bViewportRestored = false;
        bool bUsedOffscreenComposite = false;
        // True only when FSlateApplication::TakeScreenshot supplied the pixels. False means
        // either the fixed-size off-screen composite or the scene-only ReadPixels fallback.
        bool bUsedNativeBackBuffer = false;
        float DpiScale = 1.0f;
        bool bExposureApplied = false;
        bool bExposureRestored = false;
        int32 ExposureViewCount = 0;
        // FlushBeforeReadback ran on this capture. See its declaration for what that does and
        // does not establish.
        bool bReadbackFlushed = false;
    };

    // The ONE readback preamble. Call it on the game thread immediately before a capture path
    // asks the GPU for pixels; every capture verb in the plugin goes through this rather than
    // writing its own flush.
    //
    // WHAT IT DOES. FlushRenderingCommands() drains the render command queue and issues
    // ImmediateFlush(EImmediateFlushType::FlushRHIThreadFlushResources), which also flushes the
    // RHI thread and its pending resource deletions. So every resource the preceding draws
    // created, resized or released has reached a settled state before the readback's copy is set
    // up, instead of the copy racing a deletion or a descriptor update still in flight.
    //
    // WHY. The reported failure it is aimed at is a GPU page fault at a readback -- Aftermath
    // reporting AddressTranslationError / Read against a fragment shader -- which is the shape of
    // a resource-lifetime or descriptor-residency fault around the copy. This is a PLAUSIBLE
    // mitigation for that shape, NOT a proven fix: the fault was observed once, on one scene, and
    // nothing here reproduces it. Do not describe it as a crash fix anywhere.
    //
    // Returns false when it could not run (not the game thread, or no RHI), so a caller can
    // report `readbackFlushed: false` honestly instead of asserting a preamble that never ran.
    bool FlushBeforeReadback();

    // Resolve the on-disk basename for a screenshot. TWO CONTRACTS, deliberately different:
    //
    //   * A CALLER-SUPPLIED name is honoured exactly (after a .png guard and a traversal strip)
    //     and OVERWRITES whatever is already there. Callers depend on that determinism - the
    //     ortho tile grid's `<prefix>_r<row>c<col>.png` is meant to be re-written in place.
    //   * A GENERATED name (empty request, or one rejected as a traversal attempt) is UNIQUE for
    //     the lifetime of the process. It has to be: the old name carried a one-second timestamp
    //     and nothing else, so two captures issued inside the same second composed the same path,
    //     the second overwrote the first, and both responses returned success with a `path` that
    //     no longer described their own pixels. See GeneratedNameStamp in the .cpp for what that
    //     cost.
    //
    // bOutGenerated, when passed, reports which of the two happened - MakeScreenshotOutputPath
    // uses it to decide whether it is allowed to pick a different name.
    FString MakeScreenshotFilename(FString RequestedFilename, const FString& DefaultPrefix,
        bool* bOutGenerated = nullptr);

    // Compose a path under Saved/Screenshots[/Subdirectory] and create the directory. A generated
    // basename is additionally checked against the directory and re-generated if it is already
    // taken, which covers a file left by an earlier session or by a second editor on the same
    // checkout; a caller-supplied basename is used as given and overwrites.
    FString MakeScreenshotOutputPath(const FString& RequestedFilename, const FString& DefaultPrefix,
        const FString& Subdirectory, FString& OutFilename);

    // Compose the on-disk path for ui.screenshot. ui.screenshot uniquely honors a
    // caller-supplied directory (RequestedPath) — unlike the canonical
    // editor.screenshot, which forces Saved/Screenshots via MakeScreenshotOutputPath —
    // but the FILENAME half must still route through MakeScreenshotFilename so the
    // .png extension is appended only when absent (case-insensitive: "shot.png" stays
    // "shot.png" instead of doubling to "shot.png.png") and path traversal in the name
    // is sanitized, matching every other screenshot handler. RequestedPath empty falls
    // back to Saved/Screenshots/WindowsEditor. OutFilename receives the resolved
    // basename (with its single .png); the returned path is standardized.
    FString MakeUiScreenshotPath(const FString& RequestedPath, const FString& RequestedFilename,
        FString& OutFilename);

    // Stamp every pixel's alpha to 255. Mandatory on any bitmap read back from a viewport or a
    // Slate window: the editor back buffer carries alpha 0 over the SCENE (only Slate-composited
    // overlays land opaque), so encoding the raw alpha yields a near-fully-transparent PNG. Whether
    // that reads as a correct image or a blank frame then depends on the consumer — one that
    // composites alpha shows nothing but the overlays, while one that re-encodes without an alpha
    // channel shows the picture fine (B-horizontal-orthographic-views-render-no-geometry). A screen
    // capture is an opaque frame and has no transparent regions to preserve, so there is one shared
    // implementation rather than a copy per capture path.
    void ForceOpaqueAlpha(TArray<FColor>& Bitmap);

    // True when every pixel of a read-back bitmap is exactly zero in all four channels —
    // the signature of a surface that was never drawn (headless / -RenderOffScreen, a
    // minimized or just-created viewport, PIE before its first present), NOT of a dark
    // frame. MUST be called BEFORE ForceOpaqueAlpha: the alpha stamp rewrites A to 0xFF on
    // every pixel, which turns a detectably-empty all-zero readback into a plausible opaque
    // black frame and destroys the only signal a caller could have used. That is how an
    // all-zero capture used to reach disk as a valid PNG with success:true.
    //
    // The all-four-channels test is deliberately the strictest possible one so a legitimately
    // dark capture is never rejected: the Slate back-buffer path composites overlays at
    // alpha 255, and the scene-only FViewport::ReadPixels path carries the scene in the RGB
    // planes even where alpha is 0, so a real frame has a non-zero byte somewhere. An empty
    // bitmap is NOT blank by this definition — callers reject that separately as a size
    // failure, and reporting "blank" for "nothing read" would mislabel the failure.
    bool IsBlankReadback(const TArray<FColor>& Bitmap);

    // Encode a raw BGRA8 bitmap (FColor is laid out B,G,R,A in memory) as PNG bytes via
    // FImageUtils::PNGCompressImageArray — the engine's canonical FColor -> PNG encoder the
    // game-viewport screenshot path shares. It deliberately does NOT route through FImageUtils::
    // ThumbnailCompressImageArray, which emits JPEG for any image >= 8x8 (engine
    // USE_JPEG_FOR_THUMBNAILS) and would write JPEG bytes into a .png output. Returns true
    // and fills OutPng on success; false (OutPng emptied) on encode failure or a
    // degenerate/mismatched bitmap (Width/Height <= 0 or fewer than Width*Height pixels).
    // Headless-callable (no GEngine, no viewport), so the PNG-not-JPEG contract is
    // unit-testable without a live PIE viewport.
    bool EncodeBitmapToPng(int32 Width, int32 Height, const TArray<FColor>& Bitmap,
        TArray<uint8>& OutPng);

    // Configure one post-process blend as a fixed physical-camera EV100. The caller owns
    // the scope and restoration; keeping the conversion here makes it unit-testable without PIE.
    void ApplyFixedEv100ToPostProcessSettings(FPostProcessSettings& Settings, float Ev100);

    // Render only a Slate widget tree into a transparent target. DrawScale is the live
    // viewport's platform-DPI geometry scale; pairing it with SGameLayerManager's inverse
    // platform-DPI factor makes the output depend on the requested game resolution, not the host monitor.
    bool RenderSlateWidgetToSrgbColors(const TSharedRef<SWidget>& Widget, FIntPoint DrawSize,
        float DrawScale, TArray<FColor>& OutColorData, FString& OutErrorCode);

    // Slate's ordinary alpha blend writes premultiplied RGB into a transparent target.
    // Composite that layer over an opaque scene in linear space.
    bool CompositePremultipliedSlateLayer(TArray<FColor>& Scene, const TArray<FColor>& Overlay);

    // Synchronously capture the active game/PIE viewport (GEngine->GameViewport) to a PNG
    // file at OutPath. Prefers a UI-inclusive capture of the game-viewport widget's window
    // back buffer via FSlateApplication::TakeScreenshot, so the live Slate/UMG overlay
    // (HUD widgets added via AddToViewport) is composited into the frame; falls back to a
    // scene-only FViewport::ReadPixels when Slate/back-buffer capture is unavailable. When
    // Options requests OutputSize, temporarily fixes the FSceneViewport render target to that
    // exact size, renders the game layer manager off-screen at the same size, composites the
    // transparent Slate/UMG layer, and restores the viewport before writing the file. A requested
    // fixed exposure is applied by a draw-scoped extension through the view family's native
    // fixed-EV100 override and matching finalized scene-view fields after ordinary post-process
    // blends; no persistent camera post-process state is changed.
    //
    // The single canonical "game viewport -> PNG file" path shared by editor.screenshot's PIE
    // branch and ui.screenshot. On failure, sets OutErrorCode and returns false; on success fills
    // OutWidth/OutHeight and optional PNG bytes/metadata.
    bool CaptureGameViewportToPngFile(const FString& OutPath, int32& OutWidth, int32& OutHeight,
        FString& OutErrorCode, TArray<uint8>* OutPngData = nullptr,
        const FGameViewportCaptureOptions* Options = nullptr,
        FGameViewportCaptureMetadata* OutMetadata = nullptr);

    // Capture a Slate widget's window back buffer, and guarantee the Slate renderer is left with
    // NO pending screenshot request when this returns. THE ONLY supported way to reach
    // FSlateApplication::TakeScreenshot from this plugin.
    //
    // WHY IT EXISTS. FSlateApplication::TakeScreenshot hands the renderer a RAW POINTER to the
    // caller's TArray<FColor> (FSlateRHIRenderer::PrepareToTakeScreenshot parks it in
    // ScreenshotState) and then asks Slate to draw that widget's window. The renderer clears the
    // state only when the window actually appeared in the pass the draw produced -
    // FSlateRHIRenderer::DrawWindows_Private clears it under `if (bScreenshotProcessed)`, and that
    // flag is set only where the target window's FSlateViewportInfo is found in WindowsToRender.
    // A window that is hidden, minimized, sized zero, or pre-empted by an active modal window is
    // skipped by FSlateApplication::DrawWindowAndChildren, so nothing clears the state: the
    // renderer stays armed with a pointer into a frame that is about to die, and the readback
    // executes on whatever LATER Slate frame finally draws that window. On the render thread that
    // lands as an access violation inside RHIReadSurfaceData's TArray::SetNumUninitialized, on a
    // frame with no call of ours anywhere near it - which is why the crash reads as unattributable.
    //
    // TWO PROPERTIES MAKE THAT UNREACHABLE, and both belong here rather than at four call sites:
    //  1. The destination is a single buffer with STATIC STORAGE DURATION owned by this util,
    //     never a caller's local. It is the only address the plugin ever hands the renderer, so it
    //     stays valid for as long as the renderer could dereference it. Pixels are moved into
    //     OutColorData only after the readback has completed.
    //  2. On UE 5.5 and later the state is disarmed on EVERY exit path, by re-arming it at a null
    //     window - which no FSlateViewportInfo can ever match, so no draw pass can run the
    //     readback. It is unconditional because the engine exposes no way to read ScreenshotState
    //     back, and it is a re-arm rather than a clear because PrepareToTakeScreenshot check()s a
    //     non-null buffer. Through 5.4 that call has no safe form (it dereferences
    //     WindowToViewportInfo.Find() and always raises bTakingAScreenShot), so property 1 carries
    //     the guarantee alone there: a leaked request writes into the live static buffer.
    //
    // Returns TRUE only when the readback COMPLETED INSIDE THIS CALL and OutColorData holds at
    // least OutSize.X * OutSize.Y pixels; the engine flushes rendering commands itself before
    // clearing the state, so a true return means the pixels are already CPU-side and nothing is
    // still in flight. FALSE means no pixels were produced and OutColorData is left empty - never
    // that a capture is still pending. OutSize carries the rect Slate measured either way.
    // Game thread only.
    bool TakeSlateScreenshot(const TSharedRef<SWidget>& Widget, TArray<FColor>& OutColorData,
        FIntVector& OutSize);

    // The invariant TakeSlateScreenshot must never break: false on return from every exit path.
    // True only between arming the renderer and disarming it, i.e. while the call is on the stack.
    bool HasPendingSlateScreenshotRequest();

    // Pixels sitting in the process-lifetime destination buffer. Always zero once
    // TakeSlateScreenshot has returned; a non-zero count afterwards means a readback landed there
    // from a later Slate frame, which is the exact failure this util exists to make impossible.
    int32 SlateScreenshotDestinationPixelCount();
}
