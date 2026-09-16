// Copyright (c) 2026 Alexander Penkin. MIT License.

// Tests for the `hideEditorSprites` capture parameter -- the show-flag state it applies, the state
// it puts back, and the wire contract that decides whether anything is written at all.
//
// WHAT THIS DEFENDS. render.capture_open_level drives the LIVE Level Editor viewport, so a capture
// that clears a show flag and does not put it back does not merely spoil the next capture: it
// changes what the user sees in their own editor window for the rest of the session. The
// applied-and-restored property is therefore the thing under test, not the pixels.
//
// WHY IT IS ASSERTED ON A BARE FEngineShowFlags. A test that captures real pixels needs a GPU, and
// a capture test that cannot get one takes a conditional-skip path and reports success WITHOUT
// running its assertions -- board ticket B-test-skips-assertions-silently, where
// PinWright.render.capture_asset_preview.PinnedCapturesAreIdentical skipped its only substantive
// assertions in 3 of 3 runs because another project's editor held the GPU. The suite totals looked
// identical either way. So the suppression is written as pure functions over a value type
// (PinWrightRenderCapture::SuppressEditorSpriteShowFlags / RestoreEditorSpriteShowFlags) and
// asserted on a locally constructed flag set: no viewport, no world, no device, nothing to skip.
#include "Misc/AutomationTest.h"
#include "Handlers/Render/PreviewViewportCaptureUtils.h"
#include "Tests/TestUtils.h"

#include "Dom/JsonObject.h"
#include "ShowFlags.h"

namespace
{
    // Prefixed for the same reason as every other helper in Tests/Render: anonymous namespaces in
    // one Unity translation unit merge, so a bare name a sibling also uses is a latent ODR clash.
    TSharedPtr<FJsonObject> EditorSpritesPayload(bool bValuePresent, bool bValue)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        if (bValuePresent)
        {
            Payload->SetBoolField(TEXT("hideEditorSprites"), bValue);
        }
        return Payload;
    }
}

// ============================================================================
// The apply/restore round trip, from both starting states.
//
// The second half is the one that is easy to get wrong: a guard that restores by SETTING the flag
// back to true rather than to what it FOUND would look correct on a default viewport and would
// silently turn the icons back ON in a viewport the user had deliberately cleared (or had in game
// view). Both directions are asserted for that reason.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCaptureEditorSpritesRoundTripTest,
    "PinWright.render.editor_sprites.SuppressionClearsAndRestoresBillboardSprites",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCaptureEditorSpritesRoundTripTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightRenderCapture;

    // Starting from an ordinary editor viewport, where the flag is on.
    {
        FEngineShowFlags Flags(ESFIM_Editor);
        TestTrue(TEXT("an editor flag set starts with BillboardSprites on"),
            Flags.BillboardSprites != 0);

        const FEditorSpriteShowFlags Previous = SuppressEditorSpriteShowFlags(Flags);
        TestTrue(TEXT("the previous state reports the flag as it was"), Previous.bBillboardSprites);
        TestTrue(TEXT("suppression clears BillboardSprites"), Flags.BillboardSprites == 0);

        RestoreEditorSpriteShowFlags(Flags, Previous);
        TestTrue(TEXT("restore puts BillboardSprites back on"), Flags.BillboardSprites != 0);
    }

    // Starting from a viewport whose flag is ALREADY off -- a user in game view, or one who
    // cleared it by hand. Restore must return it to off, not to the default.
    {
        FEngineShowFlags Flags(ESFIM_Editor);
        Flags.SetBillboardSprites(false);

        const FEditorSpriteShowFlags Previous = SuppressEditorSpriteShowFlags(Flags);
        TestFalse(TEXT("the previous state reports the flag as it was (off)"),
            Previous.bBillboardSprites);
        TestTrue(TEXT("suppression is idempotent on an already-cleared flag"),
            Flags.BillboardSprites == 0);

        RestoreEditorSpriteShowFlags(Flags, Previous);
        TestTrue(TEXT("restore leaves an already-off flag off rather than turning icons back on"),
            Flags.BillboardSprites == 0);
    }

    // The read-only accessor the capture uses to record `before` must not itself write anything.
    {
        FEngineShowFlags Flags(ESFIM_Editor);
        const FEditorSpriteShowFlags Read = ReadEditorSpriteShowFlags(Flags);
        TestTrue(TEXT("reading reports the flag"), Read.bBillboardSprites);
        TestTrue(TEXT("reading does not clear the flag"), Flags.BillboardSprites != 0);
    }

    return true;
}

// ============================================================================
// The omitted parameter writes nothing, and the parse has exactly one spelling.
//
// The default is FALSE on purpose. render.capture_open_level has shipped for a long time and every
// existing caller's pixels must not change because a new parameter appeared -- a diff-based
// acceptance workflow would show a spurious change with no change in the level. Acceptance shots
// opt IN. This test is what stops that decision from being reversed by accident.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCaptureEditorSpritesParseDefaultTest,
    "PinWright.render.editor_sprites.OmittedParameterLeavesTheViewportAlone",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCaptureEditorSpritesParseDefaultTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightRenderCapture;

    TestFalse(TEXT("hideEditorSprites defaults to false when the field is absent"),
        ParseHideEditorSprites(EditorSpritesPayload(/*bValuePresent=*/false, false)));
    TestFalse(TEXT("hideEditorSprites:false is false"),
        ParseHideEditorSprites(EditorSpritesPayload(true, false)));
    TestTrue(TEXT("hideEditorSprites:true is true"),
        ParseHideEditorSprites(EditorSpritesPayload(true, true)));
    TestFalse(TEXT("a null payload is the do-nothing path, not a crash"),
        ParseHideEditorSprites(nullptr));

    // The same field read through the full request parser the capture verbs use, so the two cannot
    // come to disagree about the wire name.
    {
        FString ErrorCode;
        FString ErrorMessage;

        FViewportCaptureRequest Omitted;
        TestTrue(TEXT("an empty payload parses"), ParseViewportCaptureRequest(
            EditorSpritesPayload(false, false), Omitted, ErrorCode, ErrorMessage));
        TestFalse(TEXT("an omitted hideEditorSprites leaves the request's flag off"),
            Omitted.bHideEditorSprites);

        FViewportCaptureRequest Requested;
        TestTrue(TEXT("hideEditorSprites:true parses"), ParseViewportCaptureRequest(
            EditorSpritesPayload(true, true), Requested, ErrorCode, ErrorMessage));
        TestTrue(TEXT("hideEditorSprites:true reaches the request"),
            Requested.bHideEditorSprites);
    }

    return true;
}

// ============================================================================
// The engine fact this parameter is built on, pinned as a counterfactual.
//
// The obvious implementation is "call FEditorViewportClient::SetGameView(true) for the capture" --
// game view IS the editor's own hide-editor-decoration lever. It would not work: BillboardSprites
// is absent from FEngineShowFlags::Init entirely (UE 5.8 Runtime/Engine/Public/ShowFlags.h:394-520
// sets only the flags that differ from the memset-everything-on default), so it is ON in the game
// flag set as well as the editor one. Game view hides icons through a different mechanism --
// FPrimitiveSceneProxy::IsShown's Editor/Game branch, PrimitiveSceneProxy.cpp:1515-1545 -- with a
// much larger blast radius and no safe round trip.
//
// If a future engine starts clearing the flag in the game set, this test fails and the comment
// above (and the choice it justifies) gets revisited rather than quietly becoming wrong.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCaptureEditorSpritesGameFlagSetTest,
    "PinWright.render.editor_sprites.GameFlagSetDoesNotClearBillboardSprites",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCaptureEditorSpritesGameFlagSetTest::RunTest(const FString& Parameters)
{
    FEngineShowFlags GameFlags(ESFIM_Game);
    FEngineShowFlags EditorFlags(ESFIM_Editor);

    TestTrue(TEXT("the GAME flag set still carries BillboardSprites, so game view is not the lever"),
        GameFlags.BillboardSprites != 0);
    TestTrue(TEXT("the EDITOR flag set carries BillboardSprites"),
        EditorFlags.BillboardSprites != 0);

    // The flags game view really does differ on, asserted so the paragraph above is testable
    // rather than assertion-free prose: these are editor decoration, BillboardSprites is not.
    TestTrue(TEXT("Grid differs between the two sets"),
        (GameFlags.Grid != 0) != (EditorFlags.Grid != 0));
    TestTrue(TEXT("Splines differs between the two sets"),
        (GameFlags.Splines != 0) != (EditorFlags.Splines != 0));

    return true;
}

// ============================================================================
// The response block: what was asked for, what the pixels were drawn with, and the restore
// verdict -- plus the two warnings that exist so a silent no-op cannot be returned as a success.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCaptureEditorSpritesReportTest,
    "PinWright.render.editor_sprites.ResponseReportsAppliedAndRestored",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCaptureEditorSpritesReportTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightRenderCapture;

    // The ordinary success shape: suppression asked for, flag cleared for the frame, flag restored.
    {
        FViewportCaptureOutput Capture;
        Capture.bHideEditorSpritesRequested = true;
        Capture.bBillboardSpritesBefore = true;
        Capture.bBillboardSpritesApplied = false;
        Capture.bEditorSpritesRestored = true;

        const TSharedPtr<FJsonObject> Block = MakeEditorSpriteInfoObject(Capture);
        TestTrue(TEXT("hideRequested is reported"), Block->GetBoolField(TEXT("hideRequested")));
        TestFalse(TEXT("visible is false for a suppressed frame"),
            Block->GetBoolField(TEXT("visible")));
        TestTrue(TEXT("the before-state is reported"),
            Block->GetBoolField(TEXT("billboardSpritesBefore")));
        TestTrue(TEXT("restored is reported"), Block->GetBoolField(TEXT("restored")));
        TestFalse(TEXT("no hideWarning on a frame that was actually suppressed"),
            Block->HasField(TEXT("hideWarning")));
        TestFalse(TEXT("no restoreWarning on a restored capture"),
            Block->HasField(TEXT("restoreWarning")));
    }

    // The omitted-parameter shape: nothing asked for, nothing changed, icons may be in the frame
    // and the block says so rather than staying silent.
    {
        FViewportCaptureOutput Capture;
        const TSharedPtr<FJsonObject> Block = MakeEditorSpriteInfoObject(Capture);
        TestFalse(TEXT("hideRequested is false by default"),
            Block->GetBoolField(TEXT("hideRequested")));
        TestTrue(TEXT("visible is true when nothing was suppressed"),
            Block->GetBoolField(TEXT("visible")));
        TestTrue(TEXT("restored is true when nothing was written"),
            Block->GetBoolField(TEXT("restored")));
        TestFalse(TEXT("no hideWarning when no suppression was requested"),
            Block->HasField(TEXT("hideWarning")));
    }

    // The silent-no-op case this reporting exists to close: suppression requested and the flag is
    // still set on the frame that was drawn.
    {
        FViewportCaptureOutput Capture;
        Capture.bHideEditorSpritesRequested = true;
        Capture.bBillboardSpritesBefore = true;
        Capture.bBillboardSpritesApplied = true;
        const TSharedPtr<FJsonObject> Block = MakeEditorSpriteInfoObject(Capture);
        TestTrue(TEXT("a requested-but-not-applied suppression carries a hideWarning"),
            Block->HasField(TEXT("hideWarning")));
    }

    // The leak case: the capture did not put the flag back, which changes the user's own viewport.
    {
        FViewportCaptureOutput Capture;
        Capture.bHideEditorSpritesRequested = true;
        Capture.bBillboardSpritesBefore = true;
        Capture.bBillboardSpritesApplied = false;
        Capture.bEditorSpritesRestored = false;
        const TSharedPtr<FJsonObject> Block = MakeEditorSpriteInfoObject(Capture);
        TestFalse(TEXT("restored is false when the flag was left moved"),
            Block->GetBoolField(TEXT("restored")));
        TestTrue(TEXT("an unrestored capture carries a restoreWarning"),
            Block->HasField(TEXT("restoreWarning")));
    }

    // And the block is reachable from the shared viewport block every capture verb routes through,
    // so camera.orbit_shots and render.capture_annotated report it without knowing it exists.
    {
        FViewportCaptureOutput Capture;
        const TSharedPtr<FJsonObject> Viewport = MakeViewportInfoObject(Capture);
        TestTrue(TEXT("the viewport block carries editorSprites"),
            Viewport->HasTypedField<EJson::Object>(TEXT("editorSprites")));
    }

    return true;
}
