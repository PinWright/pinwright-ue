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
// WEB SETTLE MODEL: the game/editor action verbs run a per-tick Slate fingerprint settle
// loop (FDriveSettleDriver) because they can sample the live UI every frame. The web DOM
// cannot be sampled synchronously, so the web verbs use a different model:
//   - an action (click/type) INJECTS, then asynchronously RE-QUERIES the DOM exactly once
//     (the post-action re-observe) and diffs that against an optional pre-action baseline;
//   - wait_for POLLS the DOM on a fixed interval (FTSTicker) and evaluates the condition
//     each poll until it is met or the timeout elapses.
// There is no per-frame fingerprint loop on the web surface.
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

    // surface=web drive.click: optional baseline query, click by handle, single post-action
    // re-observe, resolve with { ok, code, diff }. The diff defaults to the compact summary
    // (full_diff=true for full lists); the observation is opt-in (observe defaults to none).
    static void ClickWeb(FHandlerContext& Ctx);

    // surface=web drive.type: optional baseline query, set value by handle, single
    // post-action re-observe, resolve with { ok, code, diff } (compact diff summary by
    // default; observation opt-in via observe).
    static void TypeWeb(FHandlerContext& Ctx);

    // surface=web drive.scroll: scroll an element by handle by `delta` wheel notches
    // (default 1), single post-action re-observe, resolve with { ok, code, diff }.
    static void ScrollWeb(FHandlerContext& Ctx);

    // surface=web drive.hover: hover an element by handle (DOM pointerover/mouseenter),
    // single post-action re-observe, resolve with { ok, code, diff }.
    static void HoverWeb(FHandlerContext& Ctx);

    // surface=web drive.key: dispatch a key event named `key` (a DOM key name, not an FKey),
    // optionally focusing `handle` first, with optional `modifiers` and `action` (default
    // press); single post-action re-observe, resolve with { ok, code, diff }.
    static void KeyWeb(FHandlerContext& Ctx);

    // surface=web drive.drag: drag the `handle` element onto the `to_handle` element. Web drag
    // is DOM-element-to-element (to_handle is required; coordinate to_x/to_y are not used), so
    // a missing to_handle is a synchronous INVALID_ARGUMENT. Single post-action re-observe,
    // resolve with { ok, code, diff }.
    static void DragWeb(FHandlerContext& Ctx);

    // surface=web drive.wait_for: poll the DOM on an interval, evaluating the condition each
    // poll, until met or timeout_ms; resolve with { met, elapsed_ms, matched? }. The full
    // post-wait observation is opt-in (observe defaults to none).
    static void WaitForWeb(FHandlerContext& Ctx);
};
