// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Handlers/Drive/DriveTypes.h"
#include "Handlers/Drive/DriveSetOfMarkLayout.h"
#include "Handlers/Drive/DriveEditorChrome.h"  // FDriveWindowSelector

// Renders a Set-of-Mark overlay onto a live capture: grabs the current frame (the
// game/PIE viewport, or a selected editor window), filters the supplied elements to
// interactables, lays out numbered marks (FDriveSetOfMarkLayout), paints box outlines
// plus numeric badges, then PNG and base64 encodes the annotated frame into an
// FDriveScreenshot. The drawing step (DrawMarks) is a pure function over a raw FColor
// buffer with no engine I/O, so it is unit-testable on a synthetic bitmap. The capture
// source is chosen by the surface argument: EditorChrome captures the selected window;
// Game and Web both read the game/PIE viewport (the CEF HUD composites into it via OSR, so
// the web surface reuses the game ReadPixels path and only the marks come from web rects).
class FDriveSetOfMarkRenderer
{
public:
    // Default cap on how many interactables are eligible for a visible badge; the
    // rest are reported omitted rather than silently dropped.
    static constexpr int32 DefaultMarkCap = 50;

    // Captures the live frame for `Surface` (Game and Web: the active game/PIE viewport -
    // the CEF HUD composites into it via OSR, so Web reuses the game capture; EditorChrome:
    // the window picked by `WindowSelector` via FDriveEditorChrome::
    // CaptureWindow), overlays Set-of-Mark badges for the interactable subset of
    // `Elements`, and fills `OutScreenshot` (Mime image/png, Width/Height, MarksDrawn,
    // MarksOmitted). `MarkCap <= 0` falls back to DefaultMarkCap. Returns false with
    // `OutErrorCode` set (NO_VIEWPORT / CAPTURE_FAILED / ENCODE_FAILED, or the
    // window-resolution codes for editor chrome) on failure; on success clears
    // `OutErrorCode`. Game-thread only (reads the live viewport / window). The Surface /
    // WindowSelector default to the game viewport so existing game-only callers are
    // unchanged. When `bWriteToFile` is true the encoded PNG is written to disk and
    // OutScreenshot.Path is set (Base64 left empty) instead of inlining the base64 blob;
    // a write failure returns false with OutErrorCode=WRITE_FAILED.
    static bool CaptureAnnotated(
        const TArray<FDriveElement>& Elements,
        int32 MarkCap,
        FDriveScreenshot& OutScreenshot,
        FString& OutErrorCode,
        EDriveSurface Surface = EDriveSurface::Game,
        const FDriveWindowSelector& WindowSelector = FDriveWindowSelector(),
        bool bWriteToFile = false);

    // Deliver already-encoded PNG bytes into OutScreenshot in one of two mutually-exclusive
    // shapes: when bWriteToFile, write them to a Saved/Screenshots/Drive PNG (reusing the
    // shared PinWrightScreenshotUtils path helper) and set OutScreenshot.Path with Base64
    // cleared; otherwise base64-encode into OutScreenshot.Base64 with Path cleared. Returns
    // false with OutErrorCode=WRITE_FAILED only when the disk write fails. Split out from
    // CaptureAnnotated as the pure delivery seam so the file-vs-inline contract is unit-
    // testable without a live viewport.
    static bool DeliverScreenshotBytes(
        const TArray<uint8>& PngData,
        bool bWriteToFile,
        FDriveScreenshot& OutScreenshot,
        FString& OutErrorCode);

    // Pure overlay paint over a row-major FColor buffer (Pixels[Y * Width + X]).
    // Draws each layout mark's clamped box outline and its number using a
    // self-contained 3x5 bitmap font on a contrasting badge anchored at
    // Mark.LabelAnchor. No engine dependency; every write is clamped to the frame,
    // so an empty layout leaves the buffer untouched. The mark numbers are read
    // straight from FDriveMark::Number, so any gaps in the (1-based) numbering are
    // preserved exactly.
    static void DrawMarks(
        TArray<FColor>& Pixels,
        int32 Width,
        int32 Height,
        const FDriveMarkLayout& Layout);

    // Overlay colors, exposed for verification: the box outline + badge fill, and
    // the digit ink painted on top of the badge.
    static FColor MarkColor();
    static FColor LabelColor();
};
