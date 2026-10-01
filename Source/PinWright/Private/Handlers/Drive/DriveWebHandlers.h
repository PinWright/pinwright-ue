// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"

class FHandlerContext;

// Asynchronous WEB (CEF/WebUI) branch of the drive.* handlers. The synchronous
// drive.observe / drive.expect / drive.click / drive.type / drive.wait_for handlers
// branch here when surface=web; everything below stays on their existing game/editor
// path. Each entry point takes the live FHandlerContext, opens an async response token
// (Ctx.MakeAsyncToken), drives FDriveWebBridge, and resolves the token from the bridge
// callback. Unlike the game/editor surfaces (which observe Slate synchronously), the web
// surface is fully asynchronous: every observation and action is a CEF console round-trip.
//
// "No live browser" is always a SYNCHRONOUS WEB_BROWSER_NOT_FOUND error (sent before any
// token/round-trip), never a hang.
//
// WEB ACTION MODEL (parity with the game/editor surfaces):
//   - input is REAL: the target handle is located in the page and hit-tested there
//     (document.elementFromPoint at its center; anything else on top is TARGET_OCCLUDED with
//     nothing injected), and its center is mapped to the desktop. Pointer input is routed through
//     FSlateApplication's Route* entry points along the Slate hit-test path at that point (by
//     Slate's own window order, not the platform's window-under-cursor), keys through Slate
//     keyboard focus; the browser widget forwards both to CEF, so the page sees trusted events,
//     :hover and default actions. A point whose Slate path does not reach the browser (another
//     window, or a widget drawn over it) is TARGET_OCCLUDED too.
//     drive.type is the exception: it still writes the value through the DOM.
//   - settle: the same FDriveSettleDriver decision (stable_ticks / quiet_budget_ms /
//     settle_budget_ms, or wait_for until timeout_ms) against the pre-action DOM, stepped once
//     per completed DOM query instead of once per frame, resolving the game surface's
//     { outcome, changed, settled, condition_met, elapsed_ms, ticks, input_path, diff } shape.
//   - wait_for POLLS the DOM on a fixed interval (FTSTicker) until met or timeout.
class FDriveWebHandlers
{
public:
    // surface=web drive.observe: select the browser (browser_index, default 0), query the
    // DOM once, build a Surface=Web observation (elements + optional Set-of-Mark screenshot
    // off the game viewport + optional journal), honoring interactables_only / max_elements,
    // and resolve with WriteObservation.
    static void ObserveWeb(FHandlerContext& Ctx);

    // surface=web drive.expect: one DOM query, evaluate the condition once, resolve with
    // { met, actual, expected, detail }.
    static void ExpectWeb(FHandlerContext& Ctx);

    // surface=web drive.click: locate + hit-test `handle`, real click (`button`) at its center.
    static void ClickWeb(FHandlerContext& Ctx);

    // surface=web drive.type: set the value of `handle` through the DOM (input + change events).
    static void TypeWeb(FHandlerContext& Ctx);

    // surface=web drive.scroll: locate + hit-test `handle`, real wheel of `delta` notches
    // (default 1, positive scrolls up) at its center.
    static void ScrollWeb(FHandlerContext& Ctx);

    // surface=web drive.hover: locate + hit-test `handle`, real mouse-move to its center.
    static void HoverWeb(FHandlerContext& Ctx);

    // surface=web drive.key: real key `key` (a DOM key value or an FKey name; INVALID_KEY
    // otherwise) to the browser, after locating, hit-testing and DOM-focusing the optional
    // `handle`, with optional `modifiers` and `action` (default press).
    static void KeyWeb(FHandlerContext& Ctx);

    // surface=web drive.drag: real press on `handle`, interpolated moves over `duration_ms`,
    // release on `to_handle` (both located and hit-tested). to_handle is required (coordinate
    // to_x/to_y have no web meaning): a missing one is a synchronous INVALID_ARGUMENT.
    static void DragWeb(FHandlerContext& Ctx);

    // surface=web drive.wait_for: poll the DOM on an interval, evaluating the condition each
    // poll, until met or timeout_ms; resolve with { met, elapsed_ms, matched? }. The full
    // post-wait observation is opt-in (observe defaults to none).
    static void WaitForWeb(FHandlerContext& Ctx);
};
