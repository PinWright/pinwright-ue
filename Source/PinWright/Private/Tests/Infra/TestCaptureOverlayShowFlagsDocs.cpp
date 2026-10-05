// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for E-capture-viewport-overlayshowflags-overlaywarning-undocumented.
//
// Every level capture publishes viewport.overlayShowFlags and, in game view with an overlay flag
// still set, overlayShowFlags.overlayWarning (PreviewViewportCaptureUtils.cpp). No wiki page named
// the block. Fails if render.md stops documenting the fields, `measured`, the warning trigger or
// the game-view invariants that are deliberately not reported.
#include "Misc/AutomationTest.h"
#include "Tests/Infra/WikiDocTestHelpers.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRenderCaptureOverlayShowFlagsDocTest,
    "PinWright.infra.wiki_handler.Namespace.RenderCaptureOverlayShowFlags",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FRenderCaptureOverlayShowFlagsDocTest::RunTest(const FString& Parameters)
{
    FString Text;
    if (!WikiDocTestHelpers::RenderOrFail(*this, TEXT("render"), Text))
    {
        return false;
    }

    TestTrue(TEXT("render page documents viewport.overlayShowFlags"),
        Text.Contains(TEXT("**`viewport.overlayShowFlags` reports the editor overlay show flags")));
    TestTrue(TEXT("render page says measured:false is not a clean frame"),
        Text.Contains(TEXT("`measured` (`false` means the flags were never read")));
    for (const TCHAR* Field : { TEXT("`splines`"), TEXT("`editor`"), TEXT("`selection`"),
                                TEXT("`grid`"), TEXT("`volumes`"), TEXT("`lightRadius`"),
                                TEXT("`audioRadius`"), TEXT("`navigation`"), TEXT("`game`") })
    {
        TestTrue(FString::Printf(TEXT("render page lists overlay field %s"), Field),
            Text.Contains(Field));
    }
    TestTrue(TEXT("render page states when overlayWarning fires"),
        Text.Contains(TEXT("**`overlayShowFlags.overlayWarning` fires when `viewport.gameView` is true")));
    TestTrue(TEXT("render page names the unreported game-view invariants"),
        Text.Contains(TEXT("`billboardSprites`, `selectionOutline` and `modeWidgets` are not reported")));
    // A game-view toggle restores the stored game set, so the page must not prescribe one.
    TestFalse(TEXT("render page no longer prescribes toggling game view for a fresh game set"),
        Text.Contains(TEXT("Toggle game view off and on")));
    TestTrue(TEXT("render page says a toggle does not clear the flags"),
        Text.Contains(TEXT("Toggling game view does **not** clear them")));
    return true;
}
