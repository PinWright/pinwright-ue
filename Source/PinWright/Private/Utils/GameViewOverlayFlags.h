// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"

// FEditorViewportClient::EngineShowFlags. Forward-declared rather than pulling ShowFlags.h into
// every translation unit that includes this header - the value struct below is what callers hold,
// and the two files that touch the bits include ShowFlags.h themselves. This matters because
// PreviewViewportCaptureUtils.h includes this header for the by-value member on its capture
// output, and that header is included nearly everywhere in Handlers/Render.
struct FEngineShowFlags;
class FJsonObject;

// Overlay families a level viewport draws, and the EngineShowFlags bit that governs each. Read
// off a viewport client so a caller can see which overlays game view actually suppressed instead
// of assuming it suppressed all of them.
//
// `bSplines` is here because of a concrete miss: game view was enabled, the response confirmed it,
// and a water body's spline still drew a line along the river that a reviewer nearly logged as
// mid-channel foam. Spline drawing is gated on EngineShowFlags.Splines (UE 5.8
// Runtime/Engine/Private/Components/SplineComponent.cpp:3393 and :3759,
// Runtime/Landscape/Private/LandscapeSplines.cpp:428) and the game flag set has it off
// (Runtime/Engine/Public/ShowFlags.h:469) - but SetGameView reuses the CURRENT flags as the game
// set whenever EngineShowFlags.Game is already true (EditorViewportClient.cpp:7229-7240), so the
// clearing is conditional and was never verified. Now it is measured.
//
// SHARED RATHER THAN LOCAL TO editor.set_game_view, which is where it started. The frame that
// carried the river line came out of a CAPTURE, and the capture response said nothing about
// overlays at all; the disclosure sat on a different verb, in a different response, that a
// reviewer reading a PNG never has to call. Worse, game view is per-VIEWPORT state that no capture
// owns, so a confirmation from set_game_view can be true when it is read and false when the
// shutter fires. A capture is the bounded operation that can read these flags off the client that
// is about to draw, which is what CaptureEditorViewportToPng now does.
struct FGameViewOverlayShowFlags
{
    bool bSplines = false;
    bool bBillboardSprites = false;
    bool bSelection = false;
    bool bSelectionOutline = false;
    bool bGrid = false;
    bool bVolumes = false;
    bool bLightRadius = false;
    bool bAudioRadius = false;
    bool bModeWidgets = false;
    bool bNavigation = false;
    // Which of the two flag sets is installed. Included because it is the input SetGameView
    // branches on, so it explains an otherwise inexplicable set of the flags above.
    bool bGame = false;
};

// Reads the flags above off a viewport client's show flags. Pure: nothing here writes a flag.
FGameViewOverlayShowFlags PinWrightReadGameViewOverlayFlags(const FEngineShowFlags& Flags);

// Writes the flags as flat bool fields (`splines`, `billboardSprites`, ... plus `game`) into an
// existing object. The wire names are the same wherever the block appears - editor.set_game_view's
// `overlayShowFlags` and every capture's `viewport.overlayShowFlags` - so a caller does not have
// to learn two spellings for the same bit.
void PinWrightAddGameViewOverlayFlags(const TSharedPtr<FJsonObject>& Out,
    const FGameViewOverlayShowFlags& State);

// The overlay families that are still drawing, in wire spelling, comma separated; empty when none
// are. Empty is the useful case for a caller: it is the condition under which a "game view is on
// but overlays survived it" warning must NOT be raised. `bGame` is deliberately not part of this -
// it names which flag set is installed, not an overlay in the frame.
FString PinWrightDescribeVisibleGameViewOverlays(const FGameViewOverlayShowFlags& State);
