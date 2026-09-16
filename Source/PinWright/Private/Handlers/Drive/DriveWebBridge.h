// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "Math/Vector2D.h"
#include "UObject/Object.h"
#include "UObject/WeakObjectPtr.h"
#include "Handlers/Drive/DriveTypes.h"

#include "DriveWebBridge.generated.h"

class UWebBrowser;

// WebUI/CEF bridge for the "drive" capability: observe and act on the live CEF HTML
// HUD running in PIE through the engine WebBrowser API only (no project/App dependency,
// no launch flags). It discovers live, drivable UWebBrowser instances generically, injects
// JS, and reads results back by writing a base64-encoded payload into a hidden DOM node and
// polling SWebBrowser::GetSource() for it. That pull channel needs no delegate binding, so it
// survives a game HUD widget overriding RebuildWidget() and consuming the single-cast Slate
// console event (which leaves the engine UWebBrowser::OnConsoleMessage multicast dead). The
// JS-snippet builders and the JSON parsers are PURE free functions on FDriveWebBridge so they
// are unit-testable without a live browser; the discovery + injection methods need a running
// CEF browser.

// The transform that maps a page's DOM/CSS-pixel rects into the same Slate absolute
// screen space the rest of the drive system uses (FDriveElement::AbsolutePosition/Size).
struct FDriveWebViewport
{
    // The browser widget's on-screen top-left in Slate absolute space
    // (UWidget::GetCachedGeometry().GetAbsolutePosition()). JS cannot know this offset,
    // so the C++ side supplies it.
    FVector2D BrowserAbsolutePosition = FVector2D::ZeroVector;
    // CSS-pixel -> screen-pixel scale used only when the JS payload omits devicePixelRatio
    // (the owning window's DPI scale). When the payload carries devicePixelRatio it wins,
    // because that is exactly the CSS-px -> device-px ratio CEF rendered the page at.
    double FallbackScale = 1.0;
};

// Internal one-shot awaiter that PULLS a marked result out of the live page. It binds no
// delegate: it arms an FTSTicker that repeatedly calls SWebBrowser::GetSource() and, when the
// returned HTML contains the unique marker, base64-decodes the payload after it and completes.
// It roots itself so it survives GC until it resolves on the first marked source or on timeout.
// Not meant to be used directly; FDriveWebBridge owns its lifecycle.
UCLASS()
class UDriveWebSourceAwaiter : public UObject
{
    GENERATED_BODY()

public:
    // Resolve the SWebBrowser behind InBrowser, arm the GetSource poll + timeout, and root this
    // object so it survives GC until it completes. The first source whose HTML contains InMarker
    // resolves InOnDone(true, <base64-decoded payload>); the timeout (or a lost browser) resolves
    // InOnDone(false, ""). InOnDone is invoked exactly once.
    void Start(
        UWebBrowser* InBrowser,
        const FString& InMarker,
        TFunction<void(bool /*bOk*/, const FString& /*Payload*/)> InOnDone,
        double TimeoutSeconds);

private:
    // Ticker body: enforce the timeout, then kick a GetSource pull if none is in flight.
    bool HandlePoll(float DeltaTime);

    // GetSource callback: scan the page HTML for the marker; on a hit, decode and finish.
    void HandleSource(const FString& Html);

    // Idempotent teardown: drop the ticker, unroot, then invoke the callback once.
    void Finish(bool bOk, const FString& Payload);

    TWeakObjectPtr<UWebBrowser> Browser;
    FString Marker;
    TFunction<void(bool, const FString&)> OnDone;
    FTSTicker::FDelegateHandle PollHandle;
    double DeadlineSeconds = 0.0;
    bool bRequestInFlight = false;
    bool bFinished = false;
};

// Service entry point for the surface=web drive bridge. All methods are static; the live
// methods marshal nothing themselves (the FTSTicker poll and the CEF GetSource callbacks both
// run on the game thread), so callers must invoke them on the game thread.
class FDriveWebBridge
{
public:
    // ── Discovery ────────────────────────────────────────────────────────────────────
    // Every live UWebBrowser (engine base class; project subclasses are included by the
    // iterator) that is actually drivable: not a CDO/archetype, has a cached Slate widget
    // (not slate-released/pooled), a non-empty non-about: URL, and a non-zero painted
    // geometry. Survivors are ordered largest-painted-first so SelectBrowser(0) targets the
    // visible HUD rather than a stale/background browser. Stable index == position in this list.
    static TArray<UWebBrowser*> DiscoverBrowsers();
    // The Index-th discovered browser (default the first/only), or nullptr if out of range.
    static UWebBrowser* SelectBrowser(int32 Index = 0);

    // ── Async injection / await ──────────────────────────────────────────────────────
    // Inject JS that writes `<marker> + base64(JSON.stringify((<JsExpr>)))` into a hidden DOM
    // node, then resolve OnDone with the decoded JSON tail when SWebBrowser::GetSource() first
    // reports the marker, or (false, "") on timeout.
    static void ExecuteJsAwaitResult(
        UWebBrowser* Browser,
        const FString& JsExpr,
        TFunction<void(bool bOk, const FString& JsonResult)> OnDone,
        double TimeoutSeconds = 5.0);

    // Enumerate interactable + text-bearing DOM elements, convert each DOM rect to Slate
    // absolute screen space (using this browser's live geometry + the page's
    // devicePixelRatio), and resolve OnDone with the parsed FDriveElement list (Surface=Web).
    static void QueryElements(
        UWebBrowser* Browser,
        TFunction<void(bool bOk, TArray<FDriveElement> Elements)> OnDone,
        double TimeoutSeconds = 5.0);

    // Find the element by its data-pw-id handle and .click() it. OnDone receives
    // (true, "OK") on success or (false, "<code>") where code is TARGET_NOT_FOUND /
    // TARGET_CHANGED / ACTION_FAILED / TIMEOUT / MALFORMED_JSON.
    static void ClickElement(
        UWebBrowser* Browser,
        const FString& Handle,
        TFunction<void(bool bOk, FString Code)> OnDone,
        double TimeoutSeconds = 5.0);

    // Find the element by handle, focus it, set its value/text, and dispatch input+change
    // events. OnDone codes match ClickElement.
    static void TypeIntoElement(
        UWebBrowser* Browser,
        const FString& Handle,
        const FString& Text,
        TFunction<void(bool bOk, FString Code)> OnDone,
        double TimeoutSeconds = 5.0);

    // Find the element by handle and scroll it by Delta CSS pixels: dispatch a WheelEvent at the
    // element center AND apply scrollBy on the element / nearest scrollable ancestor as a fallback
    // (so it works whether or not the page wires a wheel handler). OnDone codes match ClickElement.
    static void ScrollElement(
        UWebBrowser* Browser,
        const FString& Handle,
        double Delta,
        TFunction<void(bool bOk, const FString& Code)> OnDone,
        double TimeoutSeconds = 5.0);

    // Find the element by handle and hover it: dispatch pointerover/mouseover/mouseenter/mousemove
    // at the element center. OnDone codes match ClickElement.
    static void HoverElement(
        UWebBrowser* Browser,
        const FString& Handle,
        TFunction<void(bool bOk, const FString& Code)> OnDone,
        double TimeoutSeconds = 5.0);

    // Dispatch a KeyboardEvent. If Handle is non-empty, resolve it and focus first (else target the
    // page's active element / body). Modifiers is a "+"-joined set (e.g. "ctrl+shift"); Action is
    // press (keydown+keypress+keyup) / down (keydown) / up (keyup). OnDone codes match ClickElement.
    static void KeyElement(
        UWebBrowser* Browser,
        const FString& Handle,
        const FString& Key,
        const FString& Modifiers,
        const FString& Action,
        TFunction<void(bool bOk, const FString& Code)> OnDone,
        double TimeoutSeconds = 5.0);

    // Resolve FromHandle and ToHandle and dispatch a mouse+pointer drag sequence at their centers
    // (down on from -> move over from -> move over to -> up on to). Either handle missing reports
    // TARGET_NOT_FOUND. OnDone codes match ClickElement.
    static void DragElement(
        UWebBrowser* Browser,
        const FString& FromHandle,
        const FString& ToHandle,
        TFunction<void(bool bOk, const FString& Code)> OnDone,
        double TimeoutSeconds = 5.0);

    // ── Pure helpers (unit-testable without a live browser) ──────────────────────────
    // The drivable-URL rule used by DiscoverBrowsers: a real navigated document, i.e. a
    // non-empty URL that does not start with `about:` (about:blank/stale pages are rejected).
    static bool IsDrivableUrl(const FString& Url);
    // A process-unique marker token to tag a single round-trip's DOM-written result.
    static FString MakeMarker();
    // Escape `In` so it is safe inside a single- or double-quoted JS string literal.
    static FString EscapeJsString(const FString& In);
    // `(function(){<writer>try{__pwwrite((expr));}catch(e){...}})();` — writes
    // marker+base64(JSON.stringify(expr)) into the hidden `__pwdrive_result` node.
    static FString BuildAwaitExprJs(const FString& Marker, const FString& JsExpr);
    // Full snippet: collect interactable + text elements, stamp data-pw-id handles, write a
    // marker+base64({devicePixelRatio,innerWidth,innerHeight,elements:[...]}) into the result node.
    static FString BuildQueryElementsJs(const FString& Marker);
    // Full snippet: querySelector by handle, visibility-check, .click(), write {ok,code,...}.
    static FString BuildClickElementJs(const FString& Marker, const FString& Handle);
    // Full snippet: querySelector by handle, focus, set value, dispatch input+change.
    static FString BuildTypeIntoElementJs(const FString& Marker, const FString& Handle, const FString& Text);
    // Full snippet: querySelector by handle, visibility-check, dispatch WheelEvent at center +
    // scrollBy(el / nearest scrollable ancestor) by Delta, write {ok,code,...}.
    static FString BuildScrollElementJs(const FString& Marker, const FString& Handle, double Delta);
    // Full snippet: querySelector by handle, visibility-check, dispatch
    // pointerover/mouseover/mouseenter/mousemove at center, write {ok,code,...}.
    static FString BuildHoverElementJs(const FString& Marker, const FString& Handle);
    // Full snippet: optional querySelector+focus by handle (else activeElement/body), parse
    // Modifiers into key flags, dispatch KeyboardEvent(s) per Action, write {ok,code,...}.
    static FString BuildKeyElementJs(
        const FString& Marker, const FString& Handle, const FString& Key,
        const FString& Modifiers, const FString& Action);
    // Full snippet: querySelector both handles, dispatch mouse+pointer down/move/move/up across
    // their centers, write {ok,code,...} (TARGET_NOT_FOUND if either handle is missing).
    static FString BuildDragElementJs(const FString& Marker, const FString& FromHandle, const FString& ToHandle);
    // `(function(){...removeChild...})();` — removes the hidden `__pwdrive_result` result node
    // so PinWright leaves no persistent base64 result blob in the host page after a round-trip.
    // Removing it is safe: BuildResultWriterJs is create-if-missing, so the next query re-creates
    // the node. NOTE: the `data-pw-id` attributes stamped onto elements PERSIST by design — they
    // are the handle scheme, so a later observe/click must re-find the element by `data-pw-id` —
    // and are therefore intentionally NOT cleaned up here; a future `drive.web_reset` could strip
    // them on session end (not built now).
    static FString BuildCleanupJs();
    // If `Html` contains `Marker`, copy the base64 run immediately after it (stopping at the
    // first non-base64 char, e.g. the result node's closing tag) into OutPayload and return true.
    static bool ExtractMarkedPayload(const FString& Html, const FString& Marker, FString& OutPayload);
    // Parse a BuildQueryElementsJs payload into FDriveElements, applying the DOM->screen
    // conversion via Viewport. Returns false (with OutError) on malformed JSON or a JS-side
    // error; a well-formed payload with zero elements is a success with an empty list.
    static bool ParseQueryResult(
        const FString& Json,
        const FDriveWebViewport& Viewport,
        TArray<FDriveElement>& OutElements,
        FString& OutError);
    // Parse an action payload ({ok,code,detail}). Returns false only on malformed JSON
    // (OutCode = "MALFORMED_JSON"); otherwise fills bOutOk/OutCode/OutDetail and returns true.
    static bool ParseActionResult(const FString& Json, bool& bOutOk, FString& OutCode, FString& OutDetail);
};
