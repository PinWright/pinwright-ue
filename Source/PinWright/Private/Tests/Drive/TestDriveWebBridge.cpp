// Copyright (c) 2026 Alexander Penkin. MIT License.

// Tests for FDriveWebBridge: pure unit tests for the JS-snippet builders, the marker+base64
// extractor, the JS-string escaper, the drivable-URL discovery filter, and the result-JSON
// parsers (including the DOM->screen rect conversion). Discovery + live round-trips need a
// running CEF browser in PIE, so the one live test skips (with a warning) when
// DiscoverBrowsers() is empty - the same skip shape the live-resolver tests use when there is
// no PIE viewport to walk.

#include "Misc/AutomationTest.h"

#include "Containers/StringConv.h"
#include "Handlers/Drive/DriveTypes.h"
#include "Handlers/Drive/DriveWebBridge.h"
#include "Layout/Geometry.h"
#include "Misc/Base64.h"
#include "Tests/TestSkipReporting.h"
#include "WebBrowser.h"

namespace
{
    bool NearlyEqual(double A, double B)
    {
        return FMath::IsNearlyEqual(A, B, 0.001);
    }
}

// ============================================================================
// Pure: JS-snippet builders
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveWebQueryJsSnippetTest,
    "PinWright.drive.web.QueryJsSnippet",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FDriveWebQueryJsSnippetTest::RunTest(const FString& Parameters)
{
    const FString Marker = TEXT("__MARK__");
    const FString Js = FDriveWebBridge::BuildQueryElementsJs(Marker);

    TestTrue(TEXT("query snippet embeds the marker"), Js.Contains(Marker));
    TestTrue(TEXT("query snippet writes into the hidden result node"), Js.Contains(TEXT("__pwdrive_result")));
    TestTrue(TEXT("query snippet sets node textContent"), Js.Contains(TEXT("textContent")));
    TestTrue(TEXT("query snippet base64-encodes the payload"), Js.Contains(TEXT("btoa")));
    TestTrue(TEXT("query snippet serializes JSON"), Js.Contains(TEXT("JSON.stringify")));
    TestTrue(TEXT("query snippet selects interactable elements"), Js.Contains(TEXT("[role=button]")));
    TestTrue(TEXT("query snippet reads geometry"), Js.Contains(TEXT("getBoundingClientRect")));
    TestTrue(TEXT("query snippet stamps stable handles"), Js.Contains(TEXT("data-pw-id")));
    TestTrue(TEXT("query snippet reports devicePixelRatio"), Js.Contains(TEXT("devicePixelRatio")));

    return true;
}

// Regression: the interactable selector must cover custom ARIA/tabindex controls, not just native
// controls + role=button. Field repro: a <div class="pdd" tabindex="0" role="listbox"> custom
// dropdown matched neither the interactable selector (IS) nor the text selector (TS), so add() was
// never called on it and it was emitted with NO clickable handle - drive.click had nothing to
// target. This asserts the produced query JS (production BuildQueryElementsJs, not a copy) now
// classifies focusable [tabindex] elements and interactive ARIA roles as interactable. If the IS
// selector is reverted to the native-only set, every new assertion below fails.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveWebQueryJsInteractableSelectorTest,
    "PinWright.drive.web.QueryJsInteractableSelector",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FDriveWebQueryJsInteractableSelectorTest::RunTest(const FString& Parameters)
{
    const FString Marker = TEXT("__MARK__");
    const FString Js = FDriveWebBridge::BuildQueryElementsJs(Marker);

    // Focusable-container catch: a custom control with tabindex is user-focusable and must be
    // treated as interactable. tabindex="-1" is programmatic-only (not user-tab-navigable), so it
    // is deliberately excluded to avoid over-emitting non-interactive focus targets.
    TestTrue(TEXT("selector excludes programmatic-only tabindex=-1"),
        Js.Contains(TEXT("[tabindex]:not([tabindex=\"-1\"])")));

    // The exact custom-dropdown role from the field repro (<div ... role="listbox">) plus the rest
    // of the interactive ARIA widget roles the ticket calls out.
    TestTrue(TEXT("selector covers role=listbox (the custom dropdown from the repro)"),
        Js.Contains(TEXT("[role=listbox]")));
    TestTrue(TEXT("selector covers role=combobox"), Js.Contains(TEXT("[role=combobox]")));
    TestTrue(TEXT("selector covers role=option"), Js.Contains(TEXT("[role=option]")));
    TestTrue(TEXT("selector covers role=menuitem"), Js.Contains(TEXT("[role=menuitem]")));
    TestTrue(TEXT("selector covers role=tab"), Js.Contains(TEXT("[role=tab]")));
    TestTrue(TEXT("selector covers role=switch"), Js.Contains(TEXT("[role=switch]")));
    TestTrue(TEXT("selector covers role=checkbox"), Js.Contains(TEXT("[role=checkbox]")));
    TestTrue(TEXT("selector covers role=radio"), Js.Contains(TEXT("[role=radio]")));

    // The pre-existing native + role=button coverage must remain intact (no regression).
    TestTrue(TEXT("selector still covers role=button"), Js.Contains(TEXT("[role=button]")));
    TestTrue(TEXT("selector still covers native controls"),
        Js.Contains(TEXT("a,button,input,select,textarea")));

    // Interactable matches are marked interactable:true via add(el,true); text matches use false.
    TestTrue(TEXT("interactable matches are stamped interactable:true"),
        Js.Contains(TEXT("add(el,true)")));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveWebClickJsSnippetTest,
    "PinWright.drive.web.ClickJsSnippet",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FDriveWebClickJsSnippetTest::RunTest(const FString& Parameters)
{
    const FString Marker = TEXT("__MARK__");
    const FString Js = FDriveWebBridge::BuildClickElementJs(Marker, TEXT("pw-7"));

    TestTrue(TEXT("click snippet embeds the marker"), Js.Contains(Marker));
    TestTrue(TEXT("click snippet targets the handle"), Js.Contains(TEXT("pw-7")));
    TestTrue(TEXT("click snippet resolves by data-pw-id"), Js.Contains(TEXT("data-pw-id")));
    TestTrue(TEXT("click snippet clicks the element"), Js.Contains(TEXT(".click(")));
    TestTrue(TEXT("click snippet reports TARGET_NOT_FOUND"), Js.Contains(TEXT("TARGET_NOT_FOUND")));
    TestTrue(TEXT("click snippet reports TARGET_CHANGED"), Js.Contains(TEXT("TARGET_CHANGED")));
    TestTrue(TEXT("click snippet writes result via the hidden node"), Js.Contains(TEXT("__pwdrive_result")));
    TestTrue(TEXT("click snippet base64-encodes the payload"), Js.Contains(TEXT("btoa")));

    // A handle with a single quote must be escaped so the JS string literal stays well-formed.
    const FString Escaped = FDriveWebBridge::BuildClickElementJs(Marker, TEXT("a'b"));
    TestTrue(TEXT("click snippet escapes a quote in the handle"), Escaped.Contains(TEXT("a\\'b")));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveWebTypeJsSnippetTest,
    "PinWright.drive.web.TypeJsSnippet",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FDriveWebTypeJsSnippetTest::RunTest(const FString& Parameters)
{
    const FString Marker = TEXT("__MARK__");
    const FString Js = FDriveWebBridge::BuildTypeIntoElementJs(Marker, TEXT("pw-3"), TEXT("he\"llo\nworld"));

    TestTrue(TEXT("type snippet embeds the marker"), Js.Contains(Marker));
    TestTrue(TEXT("type snippet targets the handle"), Js.Contains(TEXT("pw-3")));
    TestTrue(TEXT("type snippet focuses the element"), Js.Contains(TEXT("focus(")));
    TestTrue(TEXT("type snippet dispatches input event"), Js.Contains(TEXT("'input'")));
    TestTrue(TEXT("type snippet dispatches change event"), Js.Contains(TEXT("'change'")));
    // The text payload must be escaped for a JS string literal.
    TestTrue(TEXT("type snippet escapes the double quote in text"), Js.Contains(TEXT("he\\\"llo")));
    TestTrue(TEXT("type snippet escapes the newline in text"), Js.Contains(TEXT("\\n")));
    TestTrue(TEXT("type snippet writes result via the hidden node"), Js.Contains(TEXT("__pwdrive_result")));
    TestTrue(TEXT("type snippet base64-encodes the payload"), Js.Contains(TEXT("btoa")));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveWebScrollJsSnippetTest,
    "PinWright.drive.web.ScrollJsSnippet",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FDriveWebScrollJsSnippetTest::RunTest(const FString& Parameters)
{
    const FString Marker = TEXT("__MARK__");
    const FString Js = FDriveWebBridge::BuildScrollElementJs(Marker, TEXT("pw-5"), 120.0);

    TestTrue(TEXT("scroll snippet embeds the marker"), Js.Contains(Marker));
    TestTrue(TEXT("scroll snippet targets the handle"), Js.Contains(TEXT("pw-5")));
    TestTrue(TEXT("scroll snippet resolves by data-pw-id"), Js.Contains(TEXT("data-pw-id")));
    TestTrue(TEXT("scroll snippet dispatches a WheelEvent"), Js.Contains(TEXT("WheelEvent")));
    TestTrue(TEXT("scroll snippet uses the 'wheel' event type"), Js.Contains(TEXT("'wheel'")));
    TestTrue(TEXT("scroll snippet sets deltaY"), Js.Contains(TEXT("deltaY")));
    TestTrue(TEXT("scroll snippet applies scrollBy as a fallback"), Js.Contains(TEXT("scrollBy")));
    TestTrue(TEXT("scroll snippet reports TARGET_NOT_FOUND"), Js.Contains(TEXT("TARGET_NOT_FOUND")));
    TestTrue(TEXT("scroll snippet writes result via the hidden node"), Js.Contains(TEXT("__pwdrive_result")));
    TestTrue(TEXT("scroll snippet base64-encodes the payload"), Js.Contains(TEXT("btoa")));

    // A handle with a single quote must be escaped so the JS string literal stays well-formed.
    const FString Escaped = FDriveWebBridge::BuildScrollElementJs(Marker, TEXT("a'b"), -1.0);
    TestTrue(TEXT("scroll snippet escapes a quote in the handle"), Escaped.Contains(TEXT("a\\'b")));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveWebHoverJsSnippetTest,
    "PinWright.drive.web.HoverJsSnippet",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FDriveWebHoverJsSnippetTest::RunTest(const FString& Parameters)
{
    const FString Marker = TEXT("__MARK__");
    const FString Js = FDriveWebBridge::BuildHoverElementJs(Marker, TEXT("pw-8"));

    TestTrue(TEXT("hover snippet embeds the marker"), Js.Contains(Marker));
    TestTrue(TEXT("hover snippet targets the handle"), Js.Contains(TEXT("pw-8")));
    TestTrue(TEXT("hover snippet resolves by data-pw-id"), Js.Contains(TEXT("data-pw-id")));
    TestTrue(TEXT("hover snippet dispatches pointerover"), Js.Contains(TEXT("'pointerover'")));
    TestTrue(TEXT("hover snippet dispatches mouseover"), Js.Contains(TEXT("'mouseover'")));
    TestTrue(TEXT("hover snippet dispatches mouseenter"), Js.Contains(TEXT("'mouseenter'")));
    TestTrue(TEXT("hover snippet dispatches mousemove"), Js.Contains(TEXT("'mousemove'")));
    TestTrue(TEXT("hover snippet positions events at the center"), Js.Contains(TEXT("clientX")));
    TestTrue(TEXT("hover snippet reports TARGET_NOT_FOUND"), Js.Contains(TEXT("TARGET_NOT_FOUND")));
    TestTrue(TEXT("hover snippet writes result via the hidden node"), Js.Contains(TEXT("__pwdrive_result")));
    TestTrue(TEXT("hover snippet base64-encodes the payload"), Js.Contains(TEXT("btoa")));

    // A handle with a single quote must be escaped so the JS string literal stays well-formed.
    const FString Escaped = FDriveWebBridge::BuildHoverElementJs(Marker, TEXT("a'b"));
    TestTrue(TEXT("hover snippet escapes a quote in the handle"), Escaped.Contains(TEXT("a\\'b")));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveWebKeyJsSnippetTest,
    "PinWright.drive.web.KeyJsSnippet",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FDriveWebKeyJsSnippetTest::RunTest(const FString& Parameters)
{
    const FString Marker = TEXT("__MARK__");
    const FString Js = FDriveWebBridge::BuildKeyElementJs(
        Marker, TEXT("pw-9"), TEXT("Enter"), TEXT("ctrl+shift"), TEXT("press"));

    TestTrue(TEXT("key snippet embeds the marker"), Js.Contains(Marker));
    TestTrue(TEXT("key snippet targets the handle"), Js.Contains(TEXT("pw-9")));
    TestTrue(TEXT("key snippet resolves by data-pw-id"), Js.Contains(TEXT("data-pw-id")));
    TestTrue(TEXT("key snippet focuses the resolved element"), Js.Contains(TEXT("focus(")));
    TestTrue(TEXT("key snippet dispatches a KeyboardEvent"), Js.Contains(TEXT("KeyboardEvent")));
    TestTrue(TEXT("key snippet carries the key"), Js.Contains(TEXT("Enter")));
    TestTrue(TEXT("key snippet carries the raw modifier string"), Js.Contains(TEXT("ctrl+shift")));
    TestTrue(TEXT("key snippet sets the ctrlKey flag"), Js.Contains(TEXT("ctrlKey")));
    TestTrue(TEXT("key snippet sets the shiftKey flag"), Js.Contains(TEXT("shiftKey")));
    // press => keydown + keypress + keyup.
    TestTrue(TEXT("key snippet dispatches keydown"), Js.Contains(TEXT("'keydown'")));
    TestTrue(TEXT("key snippet dispatches keypress"), Js.Contains(TEXT("'keypress'")));
    TestTrue(TEXT("key snippet dispatches keyup"), Js.Contains(TEXT("'keyup'")));
    TestTrue(TEXT("key snippet falls back to the active element when no handle"),
        Js.Contains(TEXT("document.activeElement")));
    TestTrue(TEXT("key snippet writes result via the hidden node"), Js.Contains(TEXT("__pwdrive_result")));
    TestTrue(TEXT("key snippet base64-encodes the payload"), Js.Contains(TEXT("btoa")));

    // The key and modifier inputs must be escaped for a JS string literal.
    const FString Escaped = FDriveWebBridge::BuildKeyElementJs(
        Marker, TEXT("a'b"), TEXT("x'y"), TEXT("ctrl'z"), TEXT("down"));
    TestTrue(TEXT("key snippet escapes a quote in the handle"), Escaped.Contains(TEXT("a\\'b")));
    TestTrue(TEXT("key snippet escapes a quote in the key"), Escaped.Contains(TEXT("x\\'y")));
    TestTrue(TEXT("key snippet escapes a quote in the modifiers"), Escaped.Contains(TEXT("ctrl\\'z")));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveWebDragJsSnippetTest,
    "PinWright.drive.web.DragJsSnippet",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FDriveWebDragJsSnippetTest::RunTest(const FString& Parameters)
{
    const FString Marker = TEXT("__MARK__");
    const FString Js = FDriveWebBridge::BuildDragElementJs(Marker, TEXT("pw-1"), TEXT("pw-2"));

    TestTrue(TEXT("drag snippet embeds the marker"), Js.Contains(Marker));
    TestTrue(TEXT("drag snippet targets the from handle"), Js.Contains(TEXT("pw-1")));
    TestTrue(TEXT("drag snippet targets the to handle"), Js.Contains(TEXT("pw-2")));
    TestTrue(TEXT("drag snippet resolves by data-pw-id"), Js.Contains(TEXT("data-pw-id")));
    TestTrue(TEXT("drag snippet presses the mouse down"), Js.Contains(TEXT("'mousedown'")));
    TestTrue(TEXT("drag snippet moves the mouse"), Js.Contains(TEXT("'mousemove'")));
    TestTrue(TEXT("drag snippet releases the mouse"), Js.Contains(TEXT("'mouseup'")));
    // Pointer-based handlers get the matching pointer sequence too.
    TestTrue(TEXT("drag snippet dispatches pointerdown"), Js.Contains(TEXT("'pointerdown'")));
    TestTrue(TEXT("drag snippet dispatches pointerup"), Js.Contains(TEXT("'pointerup'")));
    TestTrue(TEXT("drag snippet reports TARGET_NOT_FOUND when a handle is missing"),
        Js.Contains(TEXT("TARGET_NOT_FOUND")));
    TestTrue(TEXT("drag snippet writes result via the hidden node"), Js.Contains(TEXT("__pwdrive_result")));
    TestTrue(TEXT("drag snippet base64-encodes the payload"), Js.Contains(TEXT("btoa")));

    // Both handles must be escaped for a JS string literal.
    const FString Escaped = FDriveWebBridge::BuildDragElementJs(Marker, TEXT("a'b"), TEXT("c'd"));
    TestTrue(TEXT("drag snippet escapes a quote in the from handle"), Escaped.Contains(TEXT("a\\'b")));
    TestTrue(TEXT("drag snippet escapes a quote in the to handle"), Escaped.Contains(TEXT("c\\'d")));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveWebAwaitExprJsSnippetTest,
    "PinWright.drive.web.AwaitExprJsSnippet",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FDriveWebAwaitExprJsSnippetTest::RunTest(const FString& Parameters)
{
    const FString Marker = TEXT("__MARK__");
    const FString Js = FDriveWebBridge::BuildAwaitExprJs(Marker, TEXT("document.title"));

    TestTrue(TEXT("await-expr snippet embeds the marker"), Js.Contains(Marker));
    TestTrue(TEXT("await-expr snippet wraps the expression"), Js.Contains(TEXT("document.title")));
    TestTrue(TEXT("await-expr snippet serializes JSON"), Js.Contains(TEXT("JSON.stringify")));
    TestTrue(TEXT("await-expr snippet base64-encodes the payload"), Js.Contains(TEXT("btoa")));
    TestTrue(TEXT("await-expr snippet writes into the hidden result node"), Js.Contains(TEXT("__pwdrive_result")));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveWebCleanupJsSnippetTest,
    "PinWright.drive.web.CleanupJsSnippet",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FDriveWebCleanupJsSnippetTest::RunTest(const FString& Parameters)
{
    const FString Js = FDriveWebBridge::BuildCleanupJs();

    // The cleanup must target the result node by its fixed id and remove it from the DOM, so no
    // persistent base64 result blob is left in the host page after a round-trip.
    TestTrue(TEXT("cleanup snippet targets the result node by id"),
        Js.Contains(TEXT("getElementById('__pwdrive_result')")));
    TestTrue(TEXT("cleanup snippet removes the node from the DOM"), Js.Contains(TEXT("removeChild")));
    // Cleanup only tears the node down; the writer (BuildResultWriterJs) re-creates it next query,
    // so the cleanup snippet itself must not create or stamp anything.
    TestFalse(TEXT("cleanup snippet does not create a node"), Js.Contains(TEXT("createElement")));
    TestFalse(TEXT("cleanup snippet does not touch data-pw-id handles"), Js.Contains(TEXT("data-pw-id")));

    return true;
}

// ============================================================================
// Pure: JS-string escaping
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveWebEscapeJsStringTest,
    "PinWright.drive.web.EscapeJsString",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FDriveWebEscapeJsStringTest::RunTest(const FString& Parameters)
{
    const FString Escaped = FDriveWebBridge::EscapeJsString(TEXT("a'b\"c\\d\ne"));

    TestTrue(TEXT("single quote escaped"), Escaped.Contains(TEXT("a\\'b")));
    TestTrue(TEXT("double quote escaped"), Escaped.Contains(TEXT("b\\\"c")));
    TestTrue(TEXT("backslash escaped"), Escaped.Contains(TEXT("c\\\\d")));
    TestTrue(TEXT("newline escaped"), Escaped.Contains(TEXT("\\n")));
    // No raw control characters survive.
    TestFalse(TEXT("no raw newline remains"), Escaped.Contains(TEXT("\n")));

    return true;
}

// ============================================================================
// Pure: marker extraction
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveWebExtractMarkedPayloadTest,
    "PinWright.drive.web.ExtractMarkedPayload",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FDriveWebExtractMarkedPayloadTest::RunTest(const FString& Parameters)
{
    const FString Marker = TEXT("__MARK__");
    const FString Json = TEXT("{\"ok\":true}");

    // base64 of the UTF-8 bytes of Json — exactly what the page's
    // btoa(unescape(encodeURIComponent(JSON.stringify(...)))) writes into the result node.
    const FTCHARToUTF8 Utf8(*Json);
    const FString B64 = FBase64::Encode(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());

    // The marker+base64 lives inside a hidden node, wrapped by the rest of the serialized page.
    const FString Html = FString::Printf(
        TEXT("<html><body><div id=\"__pwdrive_result\" style=\"display:none\">%s%s</div></body></html>"),
        *Marker, *B64);

    FString Payload;
    const bool bFound = FDriveWebBridge::ExtractMarkedPayload(Html, Marker, Payload);
    TestTrue(TEXT("marked HTML is recognized"), bFound);
    TestEqual(TEXT("payload is exactly the base64 run (closing tag trimmed)"), Payload, B64);

    // End-to-end: the extracted base64 decodes back to the original JSON, which the action
    // parser (the same downstream parser the live path feeds) accepts.
    TArray<uint8> Bytes;
    TestTrue(TEXT("payload base64-decodes"), FBase64::Decode(Payload, Bytes));
    const auto Conv = StringCast<TCHAR>(reinterpret_cast<const UTF8CHAR*>(Bytes.GetData()), Bytes.Num());
    FString Decoded;
    Decoded.AppendChars(Conv.Get(), Conv.Length());
    TestEqual(TEXT("decoded payload is the original JSON"), Decoded, Json);

    bool bOk = false;
    FString Code;
    FString Detail;
    TestTrue(TEXT("decoded JSON parses as an action result"),
        FDriveWebBridge::ParseActionResult(Decoded, bOk, Code, Detail));
    TestTrue(TEXT("decoded action ok flag is true"), bOk);

    FString Unmatched;
    const bool bMissing = FDriveWebBridge::ExtractMarkedPayload(
        TEXT("<html><body>an unrelated page</body></html>"), Marker, Unmatched);
    TestFalse(TEXT("unmarked HTML is rejected"), bMissing);

    return true;
}

// ============================================================================
// Pure: drivable-URL discovery filter
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveWebDrivableUrlFilterTest,
    "PinWright.drive.web.DrivableUrlFilter",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FDriveWebDrivableUrlFilterTest::RunTest(const FString& Parameters)
{
    // The discovery filter must reject empty and about:* browsers (the stale/blank index-0
    // instances FIX B exists to skip), and accept real navigated pages.
    TestFalse(TEXT("empty URL is not drivable"), FDriveWebBridge::IsDrivableUrl(FString()));
    TestFalse(TEXT("about:blank is not drivable"), FDriveWebBridge::IsDrivableUrl(TEXT("about:blank")));
    TestFalse(TEXT("about: prefix is rejected case-insensitively"),
        FDriveWebBridge::IsDrivableUrl(TEXT("ABOUT:Blank")));
    TestTrue(TEXT("http page is drivable"),
        FDriveWebBridge::IsDrivableUrl(TEXT("http://localhost:8080/page.html")));
    TestTrue(TEXT("file page is drivable"),
        FDriveWebBridge::IsDrivableUrl(TEXT("file:///C:/hud/index.html")));

    return true;
}

// ============================================================================
// Pure: query-result parsing + DOM->screen conversion
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveWebParseQueryResultTest,
    "PinWright.drive.web.ParseQueryResult",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FDriveWebParseQueryResultTest::RunTest(const FString& Parameters)
{
    const FString Json = TEXT(
        "{\"devicePixelRatio\":2.0,\"innerWidth\":800,\"innerHeight\":600,\"elements\":["
        "{\"handle\":\"pw-0\",\"tag\":\"button\",\"text\":\"Start\",\"interactable\":true,"
        "\"enabled\":true,\"visible\":true,\"x\":10,\"y\":20,\"w\":30,\"h\":40},"
        "{\"handle\":\"pw-1\",\"tag\":\"span\",\"text\":\"Score\",\"interactable\":false,"
        "\"enabled\":true,\"visible\":true,\"x\":5,\"y\":6,\"w\":7,\"h\":8}]}");

    FDriveWebViewport Viewport;
    Viewport.BrowserAbsolutePosition = FVector2D(100.0, 50.0);
    Viewport.FallbackScale = 1.0;  // unused: payload carries devicePixelRatio=2.0

    TArray<FDriveElement> Elements;
    FString Error;
    const bool bParsed = FDriveWebBridge::ParseQueryResult(Json, Viewport, Elements, Error);

    TestTrue(TEXT("well-formed payload parses"), bParsed);
    TestEqual(TEXT("error is empty on success"), Error, FString());
    TestEqual(TEXT("two elements parsed"), Elements.Num(), 2);

    if (Elements.Num() == 2)
    {
        const FDriveElement& Button = Elements[0];
        TestEqual(TEXT("handle preserved"), Button.Handle, FString(TEXT("pw-0")));
        TestEqual(TEXT("tag maps to type"), Button.Type, FString(TEXT("button")));
        TestEqual(TEXT("text maps to label"), Button.Label, FString(TEXT("Start")));
        TestTrue(TEXT("interactable flag set"), Button.bInteractable);
        TestEqual(TEXT("surface is Web"),
            static_cast<int32>(Button.Surface), static_cast<int32>(EDriveSurface::Web));
        // screen.x = offset.x + dom.x * dpr = 100 + 10*2 = 120; .y = 50 + 20*2 = 90.
        TestTrue(TEXT("screen position X converted"), NearlyEqual(Button.AbsolutePosition.X, 120.0));
        TestTrue(TEXT("screen position Y converted"), NearlyEqual(Button.AbsolutePosition.Y, 90.0));
        // screen.w = dom.w * dpr = 60; .h = 80.
        TestTrue(TEXT("screen size W converted"), NearlyEqual(Button.AbsoluteSize.X, 60.0));
        TestTrue(TEXT("screen size H converted"), NearlyEqual(Button.AbsoluteSize.Y, 80.0));

        const FDriveElement& Span = Elements[1];
        TestFalse(TEXT("text-only element not interactable"), Span.bInteractable);
        TestTrue(TEXT("span screen position X converted"), NearlyEqual(Span.AbsolutePosition.X, 110.0));
        TestTrue(TEXT("span screen size W converted"), NearlyEqual(Span.AbsoluteSize.X, 14.0));
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveWebParseQueryResultFallbackScaleTest,
    "PinWright.drive.web.ParseQueryResultFallbackScale",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FDriveWebParseQueryResultFallbackScaleTest::RunTest(const FString& Parameters)
{
    // No devicePixelRatio in the payload -> the viewport FallbackScale is applied instead.
    const FString Json = TEXT(
        "{\"innerWidth\":800,\"innerHeight\":600,\"elements\":["
        "{\"handle\":\"pw-0\",\"tag\":\"button\",\"text\":\"Go\",\"interactable\":true,"
        "\"x\":10,\"y\":20,\"w\":30,\"h\":40}]}");

    FDriveWebViewport Viewport;
    Viewport.BrowserAbsolutePosition = FVector2D::ZeroVector;
    Viewport.FallbackScale = 1.5;

    TArray<FDriveElement> Elements;
    FString Error;
    const bool bParsed = FDriveWebBridge::ParseQueryResult(Json, Viewport, Elements, Error);

    TestTrue(TEXT("payload without devicePixelRatio still parses"), bParsed);
    TestEqual(TEXT("one element parsed"), Elements.Num(), 1);
    if (Elements.Num() == 1)
    {
        // 10 * 1.5 = 15, 20 * 1.5 = 30; size 30*1.5=45, 40*1.5=60.
        TestTrue(TEXT("fallback scale applied to X"), NearlyEqual(Elements[0].AbsolutePosition.X, 15.0));
        TestTrue(TEXT("fallback scale applied to Y"), NearlyEqual(Elements[0].AbsolutePosition.Y, 30.0));
        TestTrue(TEXT("fallback scale applied to W"), NearlyEqual(Elements[0].AbsoluteSize.X, 45.0));
        TestTrue(TEXT("fallback scale applied to H"), NearlyEqual(Elements[0].AbsoluteSize.Y, 60.0));
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveWebParseQueryResultMalformedTest,
    "PinWright.drive.web.ParseQueryResultMalformed",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FDriveWebParseQueryResultMalformedTest::RunTest(const FString& Parameters)
{
    FDriveWebViewport Viewport;
    TArray<FDriveElement> Elements;
    FString Error;

    const bool bParsed = FDriveWebBridge::ParseQueryResult(TEXT("{ not valid json"), Viewport, Elements, Error);
    TestFalse(TEXT("malformed JSON fails cleanly"), bParsed);
    TestFalse(TEXT("error is reported"), Error.IsEmpty());
    TestEqual(TEXT("no elements on failure"), Elements.Num(), 0);

    // A JS-side error payload surfaces as a parse failure too.
    FString JsError;
    const bool bJsError = FDriveWebBridge::ParseQueryResult(
        TEXT("{\"pwError\":\"boom\",\"elements\":[]}"), Viewport, Elements, JsError);
    TestFalse(TEXT("JS-side error fails"), bJsError);
    TestTrue(TEXT("JS error message surfaced"), JsError.Contains(TEXT("boom")));

    return true;
}

// ============================================================================
// Pure: action-result parsing
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveWebParseActionResultTest,
    "PinWright.drive.web.ParseActionResult",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FDriveWebParseActionResultTest::RunTest(const FString& Parameters)
{
    bool bOk = false;
    FString Code;
    FString Detail;

    TestTrue(TEXT("ok payload parses"),
        FDriveWebBridge::ParseActionResult(TEXT("{\"ok\":true,\"code\":\"OK\"}"), bOk, Code, Detail));
    TestTrue(TEXT("ok flag is true"), bOk);
    TestEqual(TEXT("code is OK"), Code, FString(TEXT("OK")));

    TestTrue(TEXT("failure payload parses"),
        FDriveWebBridge::ParseActionResult(
            TEXT("{\"ok\":false,\"code\":\"TARGET_NOT_FOUND\"}"), bOk, Code, Detail));
    TestFalse(TEXT("ok flag is false"), bOk);
    TestEqual(TEXT("code is TARGET_NOT_FOUND"), Code, FString(TEXT("TARGET_NOT_FOUND")));

    const bool bMalformed = FDriveWebBridge::ParseActionResult(TEXT("}{"), bOk, Code, Detail);
    TestFalse(TEXT("malformed action payload fails"), bMalformed);
    TestEqual(TEXT("malformed yields MALFORMED_JSON"), Code, FString(TEXT("MALFORMED_JSON")));

    return true;
}

// ============================================================================
// Live: discovery (skips when no CEF browser is alive)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveWebDiscoverBrowsersLiveTest,
    "PinWright.drive.web.DiscoverBrowsersLive",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FDriveWebDiscoverBrowsersLiveTest::RunTest(const FString& Parameters)
{
    const TArray<UWebBrowser*> Browsers = FDriveWebBridge::DiscoverBrowsers();
    if (Browsers.Num() == 0)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-live-browser"),
            TEXT("No live UWebBrowser found (no PIE/CEF HUD); skipping web discovery assertions."));
        return true;
    }

    for (UWebBrowser* Browser : Browsers)
    {
        TestNotNull(TEXT("discovered browser is non-null"), Browser);
        if (Browser)
        {
            // The filter only keeps drivable (non-empty, non-about:) URLs.
            TestTrue(TEXT("discovered browser has a drivable URL"),
                FDriveWebBridge::IsDrivableUrl(Browser->GetUrl()));
        }
    }

    // Survivors are ordered largest-painted-first, so SelectBrowser(0) is the most usable HUD.
    for (int32 i = 1; i < Browsers.Num(); ++i)
    {
        const FVector2D Prev = Browsers[i - 1]->GetCachedGeometry().GetLocalSize();
        const FVector2D Cur = Browsers[i]->GetCachedGeometry().GetLocalSize();
        TestTrue(TEXT("discovered browsers are ordered by painted area (descending)"),
            (Prev.X * Prev.Y) >= (Cur.X * Cur.Y));
    }

    TestTrue(TEXT("SelectBrowser(0) returns the first discovered browser"),
        FDriveWebBridge::SelectBrowser(0) == Browsers[0]);
    TestNull(TEXT("out-of-range index returns null"),
        FDriveWebBridge::SelectBrowser(Browsers.Num() + 1000));

    return true;
}

// Regression: minted pw-N handles must stay disjoint from the persistent data-pw-id stamps left by
// earlier observes. Field repro: data-pw-id stamps survive across observes (BuildCleanupJs removes
// only the result node, never the stamps), but the minter reset its counter to `var n=0` every
// observe and advanced it ONLY for unstamped elements -- so the first DOM-added interactable in a
// later observe re-minted `pw-0`, duplicating the reused `pw-0` stamp already in the same response.
// drive.click/expect re-resolve by handle via querySelector('[data-pw-id="..."]'), which returns the
// FIRST document-order match, so the action silently hit the wrong (earlier) element with ok:true.
// The fix seeds the counter past the highest existing pw-N stamp before minting. This asserts the
// produced query JS (production BuildQueryElementsJs, not a copy) performs that seed; if it is
// reverted to a bare `var n=0` with no seed scan, the primary assertions below fail. Pure string
// test on the builder output -- no PIE/CEF fixture, so it runs fully headless and never skips.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveWebQueryJsHandleUniquenessSeedTest,
    "PinWright.drive.web.QueryJsHandleUniquenessSeed",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FDriveWebQueryJsHandleUniquenessSeedTest::RunTest(const FString& Parameters)
{
    const FString Marker = TEXT("__MARK__");
    const FString Js = FDriveWebBridge::BuildQueryElementsJs(Marker);

    // Locate the seed scan and the interactable sweep once; these offsets drive both the presence
    // discriminator and the ordering guard below, so the selector literal appears exactly once.
    const int32 SeedIdx = Js.Find(TEXT("querySelectorAll('[data-pw-id]')"));
    const int32 SweepIdx = Js.Find(TEXT("querySelectorAll(IS)"));

    // Primary discriminator: the minter must SCAN the existing data-pw-id stamps to seed the counter.
    // This querySelectorAll is unique to the fix -- the buggy minter only swept IS and TS selectors.
    TestTrue(TEXT("minter scans existing data-pw-id stamps to seed the counter"),
        SeedIdx != INDEX_NONE);
    // It parses the numeric suffix of each existing pw-N stamp ...
    TestTrue(TEXT("minter matches the pw-N stamp shape"), Js.Contains(TEXT("^pw-(")));
    TestTrue(TEXT("minter parses the numeric suffix"), Js.Contains(TEXT("parseInt(")));
    // ... and advances the counter monotonically past the highest existing suffix, so a newly minted
    // handle can never equal a persistent stamp already on the page.
    TestTrue(TEXT("minter advances the seed past the highest existing suffix"),
        Js.Contains(TEXT("Math.max(n,")));

    // The mint itself is preserved: unstamped elements still get pw-(n++) and are stamped for reuse.
    TestTrue(TEXT("minter still mints pw-(n++) for unstamped elements"),
        Js.Contains(TEXT("'pw-'+(n++)")));
    TestTrue(TEXT("minter still persists the freshly minted stamp on the element"),
        Js.Contains(TEXT("setAttribute('data-pw-id'")));

    // Ordering guard: the seed scan must run BEFORE the interactable sweep that mints handles. If it
    // ran after, it would seed off the very stamps it just minted and be useless.
    TestTrue(TEXT("seed scan precedes the interactable sweep"),
        SeedIdx != INDEX_NONE && SweepIdx != INDEX_NONE && SeedIdx < SweepIdx);

    return true;
}
