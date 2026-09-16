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

    const bool bReportedSprites = GameViewTestNestedBool(
        Capture.Result, TEXT("overlayShowFlags"), TEXT("billboardSprites"), bFound);
    TestTrue(TEXT("overlayShowFlags.billboardSprites is present"), bFound);
    TestTrue(TEXT("overlayShowFlags.billboardSprites equals the measured show flag"),
        bReportedSprites == (Client->EngineShowFlags.BillboardSprites != 0));

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
