// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"

// ONE vocabulary for the `renderer` field every capture verb publishes.
//
// WHY THIS FILE EXISTS. `renderer` is not decoration. Two captures made by different renderers
// are not comparable - viewport against scene capture measures 5.76% mean absolute error against
// a 1.30% viewport self-noise floor, and an ideal fitted tone LUT only closes it to 3.28% - so a
// comparison is expected to REFUSE a pair whose `renderer` values differ rather than report the
// difference as content. Spelling one renderer two ways defeats exactly that check: it makes a
// same-renderer pair read as cross-renderer. That had already happened - render.capture_ortho_tiles
// published "sceneCapture2D" while render.detect_z_fighting published "sceneCaptureComponent2D",
// both for the same offscreen USceneCaptureComponent2D.
//
// These spellings are a WIRE CONTRACT quoted by docs/wiki-src/render.md and docs/wiki-src/widget.md.
// Changing one changes stored responses; change the doc in the same edit.
//
// The sibling rule for the OTHER capture-wide field, the perspective/orthographic one: it is
// published as `projectionMode` everywhere, and `projection` is a DIFFERENT field belonging to
// editor.set_view_mode. Both rules are enforced by Tests/Render/TestCaptureRendererVocabulary.cpp,
// which scans the handler sources rather than waiting for a live capture to prove it.
namespace PinWrightCaptureRenderer
{
    // Offscreen USceneCaptureComponent2D drawing into a transient render target, read back on the
    // render thread. Never the editor viewport, so no editor exposure/show-flag state reaches it.
    // Producers: OrthoTileCaptureHandler.cpp (tile manifest + response) and ZFightingHandler.cpp
    // (`:639`), which publishes the field directly - SceneCaptureProbeUtils.h owns the probe that
    // does the rendering but never writes `renderer`, so it is not a producer.
    inline const TCHAR* const SceneCapture2D = TEXT("sceneCapture2D");

    // FViewport::ReadPixels off a live editor viewport - level editor or an asset preview.
    // Producer: PreviewViewportCaptureUtils.cpp sets FCapture::Renderer; RenderHandler.cpp,
    // AnnotatedCaptureHandler.cpp and CameraShotPlanUtils.h copy it onto the response. Those
    // sites still write the literal; adopting this constant there is a follow-up.
    inline const TCHAR* const SceneViewportReadPixels = TEXT("sceneViewportReadPixels");

    // FWidgetRenderer into a render target - a UMG preview with no scene at all.
    // Producer: Handlers/UI/WidgetDesignerScreenshotHandler.cpp (still a literal).
    inline const TCHAR* const WidgetRenderer = TEXT("widgetRenderer");

    // A composited Slate window screenshot, not a scene render.
    // Producer: Handlers/UI/WidgetDesignerScreenshotHandler.cpp (still a literal).
    inline const TCHAR* const SlateScreenshot = TEXT("slateScreenshot");
}
