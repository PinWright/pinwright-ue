// Copyright (c) 2026 Alexander Penkin. MIT License.

// TestSetGameViewOverlayReporting.cpp - editor.set_game_view's overlay report and prior value.
//
// Two defects, both of which let a caller believe a frame was clean when it was not:
//   1. The response confirmed gameView:true while a water body's spline still drew a line down
//      the river, which a reviewer nearly logged as mid-channel foam. Spline drawing is gated on
//      EngineShowFlags.Splines (UE 5.8 Runtime/Engine/Private/Components/SplineComponent.cpp:3393,
//      :3759) and SetGameView only installs a fresh game flag set on some paths
//      (Editor/UnrealEd/Private/EditorViewportClient.cpp:7229-7240), so whether that flag was
//      actually cleared was never verified. The verb now measures it.
//   2. The prior value was unreadable, so an agent could not put the viewport back.
//
// The assertions compare the response against EngineShowFlags read straight off the client, so a
// handler that publishes a hardcoded "overlays are off" block fails - which is the direction that
// matters. A test that only checked for the field's presence would pass such a handler.

// FAutomationTestBase has no bool TestEqual overload (Misc/AutomationTest.h:1985-2003), so every
// boolean comparison below is written as TestTrue(A == B) rather than risking an implicit
// promotion picking the int32/int64/float overload.
#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
// The capture-side half of the same report: FViewportCaptureOutput / MakeViewportInfoObject, and
// the shared overlay flag table both responses are built from.
#include "Handlers/Render/PreviewViewportCaptureUtils.h"
#include "Utils/GameViewOverlayFlags.h"
#include "ShowFlags.h"
#include "Tests/TestUtils.h"
#include "Tests/TestSkipReporting.h"

#include "Editor.h"
#include "EditorViewportClient.h"
#include "IAssetViewport.h"
#include "LevelEditor.h"
#include "Misc/ScopeExit.h"
#include "Modules/ModuleManager.h"

namespace
{
    FEditorViewportClient* GameViewTestClient()
    {
        if (!GEditor)
        {
            return nullptr;
        }
        FLevelEditorModule& LevelEditorModule =
            FModuleManager::GetModuleChecked<FLevelEditorModule>("LevelEditor");
        TSharedPtr<IAssetViewport> ActiveViewport = LevelEditorModule.GetFirstActiveViewport();
        if (!ActiveViewport.IsValid())
        {
            return nullptr;
        }
        return &ActiveViewport->GetAssetViewportClient();
    }

    TSharedPtr<FJsonObject> GameViewTestPayload(bool bEnabled)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetBoolField(TEXT("enabled"), bEnabled);
        return Payload;
    }

    // Reads a bool out of a named sub-object. bOutFound distinguishes "absent" from "false",
    // which is the whole difference between an unreported overlay and a suppressed one.
    bool GameViewTestNestedBool(const TSharedPtr<FJsonObject>& Result, const TCHAR* Object,
                                const TCHAR* Field, bool& bOutFound)
    {
        bOutFound = false;
        if (!Result.IsValid())
        {
            return false;
        }
        const TSharedPtr<FJsonObject>* Inner = nullptr;
        if (!Result->TryGetObjectField(Object, Inner) || !Inner || !Inner->IsValid())
        {
            return false;
        }
        bool Value = false;
        bOutFound = (*Inner)->TryGetBoolField(Field, Value);
        return Value;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSetGameViewReportsMeasuredOverlayFlagsTest,
    "PinWright.editor.set_game_view.ReportsMeasuredOverlayFlags",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSetGameViewReportsMeasuredOverlayFlagsTest::RunTest(const FString& Parameters)
{
    FEditorViewportClient* Client = GameViewTestClient();
    if (!Client)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-level-viewport"),
            TEXT("Skipped: no active level viewport, so no show flags are readable."));
        return true;
    }

    const bool bWasInGameView = Client->IsInGameView();
    ON_SCOPE_EXIT
    {
        // Game view is global viewport state; this test does not get to leave it changed.
        Client->SetGameView(bWasInGameView);
    };

    FTestResponseCapture Capture;
    TestTrue(TEXT("editor.set_game_view handler registered"),
        InvokeHandlerWithCapture(TEXT("editor.set_game_view"), GameViewTestPayload(true), Capture));
    TestTrue(TEXT("set_game_view reports success"), Capture.bSuccess);

    if (!Capture.Result.IsValid())
    {
        AddError(TEXT("set_game_view returned no result object"));
        return true;
    }

    // The prior value. Without it an agent cannot restore the viewport, which is why the verb
    // was unusable as a "leave the editor as you found it" step.
    bool bFound = false;
    const bool bReportedPrevious =
        GameViewTestNestedBool(Capture.Result, TEXT("previous"), TEXT("gameViewEnabled"), bFound);
    TestTrue(TEXT("previous.gameViewEnabled is present"), bFound);
    TestTrue(TEXT("previous.gameViewEnabled is the pre-call value"),
        bReportedPrevious == bWasInGameView);

    // Every overlay flag must equal what the client actually holds now. A block of literals
    // saying "all suppressed" fails here the moment the engine leaves one set - which is exactly
    // the case that produced the river line.
    const bool bReportedSplines =
        GameViewTestNestedBool(Capture.Result, TEXT("overlayShowFlags"), TEXT("splines"), bFound);
    TestTrue(TEXT("overlayShowFlags.splines is present"), bFound);
    TestTrue(TEXT("overlayShowFlags.splines equals the measured show flag"),
        bReportedSplines == (Client->EngineShowFlags.Splines != 0));

    // `editor` is the flag that hides editor-only sprites and icons in game view, so it is the one
    // that makes the summary's "hides billboards" claim checkable. billboardSprites is not
    // reported: it reads true in every game flag set and only ever looked like a failed toggle.
    const bool bReportedEditor = GameViewTestNestedBool(
        Capture.Result, TEXT("overlayShowFlags"), TEXT("editor"), bFound);
    TestTrue(TEXT("overlayShowFlags.editor is present"), bFound);
    TestTrue(TEXT("overlayShowFlags.editor equals the measured show flag"),
        bReportedEditor == (Client->EngineShowFlags.Editor != 0));
    GameViewTestNestedBool(Capture.Result, TEXT("overlayShowFlags"), TEXT("billboardSprites"), bFound);
    TestFalse(TEXT("overlayShowFlags carries no billboardSprites field"), bFound);

    // The coverage the verb does NOT have has to be stated, or the response reads as a blanket
    // "the viewport is clean now" claim.
    const TArray<TSharedPtr<FJsonValue>>* NotGoverned = nullptr;
    TestTrue(TEXT("notGovernedByGameView is present"),
        Capture.Result->TryGetArrayField(TEXT("notGovernedByGameView"), NotGoverned));
    if (NotGoverned)
    {
        TestTrue(TEXT("notGovernedByGameView names at least one uncovered family"),
            NotGoverned->Num() > 0);
    }

    // And the contradiction warning must appear if and only if the contradiction is real.
    const bool bSplinesStillOn = Client->EngineShowFlags.Splines != 0;
    FString Warning;
    const bool bHasWarning =
        Capture.Result->TryGetStringField(TEXT("overlayWarning"), Warning);
    if (Client->IsInGameView())
    {
        TestTrue(TEXT("overlayWarning is present exactly when splines survived game view"),
            bHasWarning == bSplinesStillOn);
        if (bHasWarning)
        {
            TestFalse(TEXT("set_game_view overlayWarning does not prescribe a game-view toggle"),
                Warning.Contains(TEXT("off and on again")));
        }
    }

    return true;
}

// ============================================================================
// The CAPTURE half of the same defect.
//
// editor.set_game_view measuring the flags is not enough, and the frame that started this proves
// it: the river line was seen in a CAPTURE, whose response carried `gameView: true` and said
// nothing about the overlays game view does not govern. A reviewer reading that PNG never has to
// call set_game_view at all, and game view is per-viewport state another agent can clear between
// that call's confirmation and the shutter. So the capture reads the flags off the client that is
// about to draw and publishes them in its own `viewport` block.
//
// ASSERTED ON PURE FUNCTIONS OVER A VALUE TYPE, for the reason spelled out at the top of
// Tests/Render/TestCaptureEditorSprites.cpp: a test that captures real pixels needs a GPU, and a
// capture test that cannot get one takes a conditional-skip path and reports success WITHOUT
// running its assertions (board ticket B-test-skips-assertions-silently). The read
// (PinWrightReadGameViewOverlayFlags) and the publish (MakeViewportInfoObject) are both pure, so
// there is nothing here to skip. The one line these cannot reach is the read INSIDE
// CaptureEditorViewportToPng, which needs a live viewport; what is asserted is that the block it
// fills reaches the response with the measured values and with the contradiction named.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCaptureReportsMeasuredOverlayShowFlagsTest,
    "PinWright.render.capture.ReportsMeasuredOverlayShowFlags",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCaptureReportsMeasuredOverlayShowFlagsTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightRenderCapture;

    // The read reports what the flag set holds, in both directions. A read that returned a fixed
    // "overlays are off" would pass a presence-only check and fail here.
    {
        FEngineShowFlags Flags(ESFIM_Editor);
        Flags.SetSplines(true);
        Flags.SetGrid(false);
        const FGameViewOverlayShowFlags State = PinWrightReadGameViewOverlayFlags(Flags);
        TestTrue(TEXT("the read reports Splines as the flag set holds it"), State.bSplines);
        TestFalse(TEXT("the read reports Grid as the flag set holds it"), State.bGrid);

        const FString Visible = PinWrightDescribeVisibleGameViewOverlays(State);
        TestTrue(TEXT("the description names the overlay that is on"),
            Visible.Contains(TEXT("splines")));
        TestFalse(TEXT("the description omits the overlay that is off"),
            Visible.Contains(TEXT("grid")));
    }

    // The block on the capture response. THIS IS THE ASSERTION THAT FAILS BEFORE THE FIX: the
    // capture's viewport block published `gameView` and nothing about the overlays it does not
    // govern, so a spline drawing into the frame was invisible to everyone but the pixels.
    {
        FViewportCaptureOutput Capture;
        Capture.bGameView = true;
        Capture.bOverlayShowFlagsMeasured = true;
        Capture.OverlayShowFlags.bSplines = true;

        const TSharedPtr<FJsonObject> Viewport = MakeViewportInfoObject(Capture);
        const TSharedPtr<FJsonObject>* Overlays = nullptr;
        if (!Viewport->TryGetObjectField(TEXT("overlayShowFlags"), Overlays) || !Overlays)
        {
            AddError(TEXT("the capture viewport block carries no overlayShowFlags, so an overlay "
                          "drawn into the frame is reported nowhere the caller reads"));
            return true;
        }

        TestTrue(TEXT("overlayShowFlags.measured is true when the capture read the client"),
            (*Overlays)->GetBoolField(TEXT("measured")));
        TestTrue(TEXT("overlayShowFlags.splines carries the measured flag"),
            (*Overlays)->GetBoolField(TEXT("splines")));
        TestFalse(TEXT("an unset overlay is reported as unset"),
            (*Overlays)->GetBoolField(TEXT("grid")));

        FString Warning;
        TestTrue(TEXT("game view plus a surviving overlay flag raises overlayWarning"),
            (*Overlays)->TryGetStringField(TEXT("overlayWarning"), Warning));
        TestTrue(TEXT("the warning names the overlay that is still drawing"),
            Warning.Contains(TEXT("splines")));
    }

    // A genuinely clean frame must NOT carry the warning. A warning on every capture is noise, and
    // noise is not read - which is how the river line survived two reviews.
    {
        FViewportCaptureOutput Capture;
        Capture.bGameView = true;
        Capture.bOverlayShowFlagsMeasured = true;

        const TSharedPtr<FJsonObject> Viewport = MakeViewportInfoObject(Capture);
        const TSharedPtr<FJsonObject>* Overlays = nullptr;
        TestTrue(TEXT("overlayShowFlags is published on every capture"),
            Viewport->TryGetObjectField(TEXT("overlayShowFlags"), Overlays) && Overlays != nullptr);
        if (Overlays)
        {
            TestFalse(TEXT("no overlayWarning when no overlay flag survived game view"),
                (*Overlays)->HasField(TEXT("overlayWarning")));
        }
    }

    // And an output built on a path that never reached the read must say so, rather than
    // publishing a set of falses that reads as a measured clean frame.
    {
        FViewportCaptureOutput Capture;
        Capture.bGameView = true;

        const TSharedPtr<FJsonObject> Viewport = MakeViewportInfoObject(Capture);
        const TSharedPtr<FJsonObject>* Overlays = nullptr;
        TestTrue(TEXT("overlayShowFlags is published even when nothing was measured"),
            Viewport->TryGetObjectField(TEXT("overlayShowFlags"), Overlays) && Overlays != nullptr);
        if (Overlays)
        {
            TestFalse(TEXT("measured is false on an output that never read the client"),
                (*Overlays)->GetBoolField(TEXT("measured")));
        }
    }

    return true;
}

// ============================================================================
// Flags that read the same after EVERY game-view enable are not evidence, and must not be
// published as if they were. BillboardSprites is never cleared for any init mode (UE 5.8
// Runtime/Engine/Public/ShowFlags.h:389-397), SetGameView forces ModeWidgets on and
// SelectionOutline and Selection off (Editor/UnrealEd/Private/EditorViewportClient.cpp:7268-7273).
// Navigation is NOT invariant (the P key toggles it inside game view and SetGameView reuses that
// stored set), so it stays published and warned about. With BillboardSprites and ModeWidgets in
// the table, a correctly installed game set described two overlays as "still drawing", so the
// capture's overlayWarning fired on every game-view frame and billboardSprites:true read as a
// failed toggle. The flag that does hide editor sprites in game view is Editor.
//
// Fails before the fix: the old table published billboardSprites/modeWidgets/selectionOutline,
// had no `editor`, and described the fresh game set below as "billboardSprites,
// modeWidgets", so the clean-frame assertions fail.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGameViewOverlayFlagsOmitInvariantFlagsTest,
    "PinWright.render.capture.GameViewOverlayFlagsOmitInvariantFlags",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGameViewOverlayFlagsOmitInvariantFlagsTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightRenderCapture;

    // The game set exactly as SetGameView(true) installs it from scratch.
    FEngineShowFlags GameFlags(ESFIM_Game);
    GameFlags.SetModeWidgets(true);
    GameFlags.SetSelection(false);
    GameFlags.SetSelectionOutline(false);
    // Preconditions: the engine still holds the invariants this fix is about.
    TestTrue(TEXT("precondition: BillboardSprites stays on in the game set"),
        GameFlags.BillboardSprites != 0);
    TestTrue(TEXT("precondition: the game set has Editor off"), GameFlags.Editor == 0);

    const FGameViewOverlayShowFlags State = PinWrightReadGameViewOverlayFlags(GameFlags);
    const FString Visible = PinWrightDescribeVisibleGameViewOverlays(State);
    TestTrue(FString::Printf(TEXT("a freshly installed game set leaves no overlay drawing (got '%s')"),
        *Visible), Visible.IsEmpty());

    TSharedPtr<FJsonObject> Block = MakeShared<FJsonObject>();
    PinWrightAddGameViewOverlayFlags(Block, State);
    for (const TCHAR* Invariant : { TEXT("billboardSprites"), TEXT("selectionOutline"),
                                    TEXT("modeWidgets") })
    {
        TestFalse(FString::Printf(TEXT("overlayShowFlags omits invariant field %s"), Invariant),
            Block->HasField(Invariant));
    }
    TestTrue(TEXT("overlayShowFlags publishes the governing `editor` flag"),
        Block->HasField(TEXT("editor")));

    // The capture over that frame stays quiet.
    FViewportCaptureOutput Capture;
    Capture.bGameView = true;
    Capture.bOverlayShowFlagsMeasured = true;
    Capture.OverlayShowFlags = State;
    const TSharedPtr<FJsonObject> Viewport = MakeViewportInfoObject(Capture);
    const TSharedPtr<FJsonObject>* Overlays = nullptr;
    if (Viewport->TryGetObjectField(TEXT("overlayShowFlags"), Overlays) && Overlays)
    {
        TestFalse(TEXT("no overlayWarning on a correctly installed game set"),
            (*Overlays)->HasField(TEXT("overlayWarning")));
    }
    else
    {
        AddError(TEXT("the capture viewport block carries no overlayShowFlags"));
    }

    // And Editor left on in game view IS an overlay in the frame: editor-only sprites draw.
    GameFlags.SetEditor(true);
    const FString WithEditor =
        PinWrightDescribeVisibleGameViewOverlays(PinWrightReadGameViewOverlayFlags(GameFlags));
    TestTrue(TEXT("Editor left on in game view is named as still drawing"),
        WithEditor.Contains(TEXT("editor")));
    GameFlags.SetEditor(false);

    // Navigation toggled on inside game view (P key) draws the navmesh, so the capture must warn.
    GameFlags.SetNavigation(true);
    FViewportCaptureOutput NavCapture;
    NavCapture.bGameView = true;
    NavCapture.bOverlayShowFlagsMeasured = true;
    NavCapture.OverlayShowFlags = PinWrightReadGameViewOverlayFlags(GameFlags);
    const TSharedPtr<FJsonObject> NavViewport = MakeViewportInfoObject(NavCapture);
    const TSharedPtr<FJsonObject>* NavOverlays = nullptr;
    FString NavWarning;
    TestTrue(TEXT("Navigation on in the game set raises overlayWarning naming navigation"),
        NavViewport->TryGetObjectField(TEXT("overlayShowFlags"), NavOverlays) && NavOverlays &&
        (*NavOverlays)->TryGetStringField(TEXT("overlayWarning"), NavWarning) &&
        NavWarning.Contains(TEXT("navigation")));

    // The remedy. A game-view toggle restores the stored game set, flag included
    // (EditorViewportClient.cpp:7236-7245), so the warning must not prescribe one, and must name
    // a way to clear the flag that works. Fails against the old "Toggle editor.set_game_view off
    // and on again to force a fresh game flag set" text.
    TestFalse(TEXT("capture overlayWarning does not prescribe toggling game view off and on"),
        NavWarning.Contains(TEXT("off and on again")) || NavWarning.Contains(TEXT("fresh game flag set")));
    TestTrue(TEXT("capture overlayWarning names the ShowFlag cvar remedy"),
        NavWarning.Contains(TEXT("\"ShowFlag.<Name> 0\"")));
    // A forced ShowFlag.* cvar is folded into the capture's overlay report, the way the engine
    // folds it into the drawn view family. Splines on in the client but forced off, Navigation off
    // in the client but forced on: the frame has the navmesh and no spline lines.
    {
        FEngineShowFlags Drawn(ESFIM_Game);
        Drawn.SetSplines(true);
        Drawn.SetNavigation(false);
        TArray<FForcedShowFlagOverride> Overrides;
        FForcedShowFlagOverride& SplinesOff = Overrides.AddDefaulted_GetRef();
        SplinesOff.Name = TEXT("Splines");
        SplinesOff.Value = 0;
        FForcedShowFlagOverride& NavigationOn = Overrides.AddDefaulted_GetRef();
        NavigationOn.Name = TEXT("Navigation");
        NavigationOn.Value = 1;
        ApplyForcedShowFlagOverrides(Drawn, Overrides);

        FViewportCaptureOutput Forced;
        Forced.bGameView = true;
        Forced.bOverlayShowFlagsMeasured = true;
        Forced.OverlayShowFlags = PinWrightReadGameViewOverlayFlags(Drawn);
        TestFalse(TEXT("ShowFlag.Splines 0 clears splines in the overlay report"),
            Forced.OverlayShowFlags.bSplines);
        TestTrue(TEXT("ShowFlag.Navigation 1 sets navigation in the overlay report"),
            Forced.OverlayShowFlags.bNavigation);
        TestTrue(TEXT("with the overrides applied only navigation is still drawing"),
            PinWrightDescribeVisibleGameViewOverlays(Forced.OverlayShowFlags) == TEXT("navigation"));
        const TSharedPtr<FJsonObject> ForcedViewport = MakeViewportInfoObject(Forced);
        const TSharedPtr<FJsonObject>* ForcedOverlays = nullptr;
        FString ForcedWarning;
        TestTrue(TEXT("the forced navmesh raises overlayWarning"),
            ForcedViewport->TryGetObjectField(TEXT("overlayShowFlags"), ForcedOverlays) &&
            ForcedOverlays && (*ForcedOverlays)->TryGetStringField(TEXT("overlayWarning"), ForcedWarning));
        TestTrue(TEXT("the warning's flag list is navigation alone"),
            ForcedWarning.Contains(TEXT("were still set: navigation.")));
    }

    const FString Remedy = PinWrightGameViewOverlayRemedy();
    TestTrue(TEXT("the shared remedy says a toggle does not clear the flag"),
        Remedy.Contains(TEXT("Toggling game view does not clear it")));

    return true;
}

// The set_game_view overlayWarning, forced to fire. The handler test above only checks the text
// when the host's game set happens to carry Splines, which it usually does not, so it was vacuous.
// Here Splines is set in the game set (SetGameView reuses it on the enable below) and the remedy
// is asserted unconditionally. Fails against the old "Toggle game view off and on again" text.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSetGameViewOverlayWarningRemedyTest,
    "PinWright.editor.set_game_view.OverlayWarningRemedyIsNotAToggle",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSetGameViewOverlayWarningRemedyTest::RunTest(const FString& Parameters)
{
    FEditorViewportClient* Client = GameViewTestClient();
    if (!Client)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-level-viewport"),
            TEXT("Skipped: no active level viewport, so set_game_view has nothing to toggle."));
        return true;
    }

    const bool bWasInGameView = Client->IsInGameView();
    Client->SetGameView(true);
    const bool bGameSetSplines = Client->EngineShowFlags.Splines != 0;
    ON_SCOPE_EXIT
    {
        // Put the game set back as it was (it persists in the viewport and the editor ini), then
        // the game-view state.
        Client->SetGameView(true);
        Client->EngineShowFlags.SetSplines(bGameSetSplines);
        Client->SetGameView(bWasInGameView);
    };
    Client->EngineShowFlags.SetSplines(true);
    TestTrue(TEXT("precondition: Splines is set in the installed game set"),
        Client->IsInGameView() && Client->EngineShowFlags.Splines != 0);

    FTestResponseCapture Capture;
    TestTrue(TEXT("editor.set_game_view handler registered"),
        InvokeHandlerWithCapture(TEXT("editor.set_game_view"), GameViewTestPayload(true), Capture));
    FString Warning;
    TestTrue(TEXT("overlayWarning fires when Splines survives game view"),
        Capture.Result.IsValid() && Capture.Result->TryGetStringField(TEXT("overlayWarning"), Warning));
    TestFalse(TEXT("overlayWarning does not prescribe toggling game view off and on"),
        Warning.Contains(TEXT("off and on again")) || Warning.Contains(TEXT("fresh game flag set")));
    TestTrue(TEXT("overlayWarning says a toggle does not clear the flag"),
        Warning.Contains(TEXT("Toggling game view does not clear it")));
    return true;
}
