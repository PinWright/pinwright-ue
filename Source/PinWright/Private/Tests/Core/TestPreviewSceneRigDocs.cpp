// Copyright (c) 2026 Alexander Penkin. MIT License.

// Doc regression tests for the previewScene capture rig (docs/preview-scene-rig.md, R6).
//
// Three things have to stay true in the rendered wiki, and each of them was wrong or missing
// before this wave:
//   1. The standalone topic page docs/wiki-src/render.preview-scene-rig.md exists and carries the
//      facts that are invisible from the generated content alone -- the arrival-angle convention,
//      the refusal of per-capture profile selection, and the real exposure hazard (the eye-adaptation
//      clear behind bPostProcessingEnabled=false, NOT the tonemapper show flag, which was the
//      audit's one refuted claim).
//   2. docs/wiki-src/render.md's `viewport` field table has a previewScene row pointing at that
//      page. That table is hand-written overlay content; the generator emits nothing like it.
//   3. The "Captures are stamped opaque on every path" promise is GONE from camera.md and
//      level-building.capture-and-review.md. It was false for widget.screenshot_designer with
//      target:"preview" and for asset.dump's widget aspect until R4 landed the stamp, and the
//      sentence outlived its own truth for months.
//
// Every marker asserted below is overlay-EXCLUSIVE. None of them is a bare parameter name: the
// generator emits "previewScene" from the verbs' RPC_PARAMS whether or not a word of prose was ever
// written, so a test asserting on that string could not fail if every overlay were deleted.
//
// NOTE FOR A RED RUN AFTER A FRESH CHECKOUT: a BRAND-NEW topic file needs an editor restart before
// WikiHandler resolves it -- the topic-node set is built once with the wiki cache
// (docs/wiki-src/README.md, "Adding a brand-new topic file requires an editor restart"). Edits to an
// existing page are mtime-revalidated and need none. If PreviewSceneRig below fails with a Not-Found
// page on the first run after this file landed, restart the editor before looking for a doc bug.
#include "Misc/AutomationTest.h"
#include "Tests/Infra/WikiDocTestHelpers.h"

// ============================================================================
// The topic page itself.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPreviewSceneRigTopicDocTest,
    "PinWright.infra.wiki_handler.TopicPage.PreviewSceneRig",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPreviewSceneRigTopicDocTest::RunTest(const FString& Parameters)
{
    FString Text;
    if (!WikiDocTestHelpers::RenderOrFail(*this, TEXT("render.preview-scene-rig"), Text))
    {
        return false;
    }

    // The arrival-angle convention, and the fact that it was derived twice. A page that only
    // stated the rotation triple would leave the next reader redoing the trig -- and the engine
    // stores the OPPOSITE convention, so getting it wrong is silent.
    TestTrue(TEXT("page states arrival azimuth 112.5 as the engine's shipped default"),
        Text.Contains(TEXT("112.5")));
    TestTrue(TEXT("page carries the independent empirical corroboration near azimuth 110"),
        Text.Contains(TEXT("110")) && Text.Contains(TEXT("Two independent methods")));
    TestTrue(TEXT("page states the angles are where the light ARRIVES from"),
        Text.Contains(TEXT("ARRIVES from")));

    // The refusal of per-capture profile selection. Without this the reader sees a missing
    // feature rather than a decision, and reaches for SetProfileIndex through property.set.
    TestTrue(TEXT("page names SetProfileIndex and refuses per-capture profile selection"),
        Text.Contains(TEXT("SetProfileIndex")) && Text.Contains(TEXT("is refused")));
    TestTrue(TEXT("page gives the component-only equivalent of the Grey Ambient profile"),
        Text.Contains(TEXT("Grey Ambient")) && Text.Contains(TEXT("no broadcast")));

    // The refuted claim must NOT be propagated, and the real hazard must be named. This is the
    // one place the plan's originating audit was wrong, so the page has to carry the correction
    // rather than the suspicion.
    TestTrue(TEXT("page names bPostProcessingEnabled as the real exposure hazard"),
        Text.Contains(TEXT("bPostProcessingEnabled")));
    TestTrue(TEXT("page states the tonemapper flag selects the gamma-only permutation, not a skipped pass"),
        Text.Contains(TEXT("gamma-only")));
    TestTrue(TEXT("page records that the tonemapper exposure-hole claim was withdrawn"),
        Text.Contains(TEXT("withdrawn")));

    // The report block: the unconditional-block / omitted-field rule, and the three restore levels.
    TestTrue(TEXT("page states the previewScene block is unconditional on every viewport-owning verb"),
        Text.Contains(TEXT("sceneAvailable")) && Text.Contains(TEXT("unconditional")));
    TestTrue(TEXT("page states fields inside the block are omitted rather than zeroed"),
        Text.Contains(TEXT("omitted, never zeroed")));
    TestTrue(TEXT("page publishes the config-file digest field of the restore"),
        Text.Contains(TEXT("restore.configFileUnchanged")));
    return true;
}

// ============================================================================
// render.md's `viewport` field table carries the row. The table is overlay content in the
// `## Check the view mode before you trust a capture` section; nothing generated resembles it.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRenderPreviewSceneRigRowDocTest,
    "PinWright.infra.wiki_handler.Namespace.RenderPreviewSceneRigRow",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FRenderPreviewSceneRigRowDocTest::RunTest(const FString& Parameters)
{
    FString Text;
    if (!WikiDocTestHelpers::RenderOrFail(*this, TEXT("render"), Text))
    {
        return false;
    }

    // The cross-link is the load-bearing half: the field table is where a caller reading a
    // capture response looks up an unfamiliar block, and a row with no destination sends them
    // back to the C++.
    TestTrue(TEXT("render page's viewport table links the preview-scene-rig topic page"),
        Text.Contains(TEXT("render.preview-scene-rig.md")));
    TestTrue(TEXT("render page names the measured profile as the field that makes two editors comparable"),
        Text.Contains(TEXT("profileName")));
    return true;
}

// ============================================================================
// The withdrawn opaque-alpha promise. Asserted on the EXACT shipped sentence fragment rather
// than a paraphrase -- a paraphrase search passes against the original text.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpaqueEveryPathClaimIsGoneDocTest,
    "PinWright.infra.wiki_handler.Namespace.OpaqueEveryPathClaimIsGone",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FOpaqueEveryPathClaimIsGoneDocTest::RunTest(const FString& Parameters)
{
    // Both spellings that shipped: level-building said "stamped opaque on every path now", camera
    // said "now stamped opaque on every path". This substring is common to both, which is why it
    // is the one asserted.
    const TCHAR* const ForbiddenClaim = TEXT("stamped opaque on every path");

    FString CameraText;
    if (!WikiDocTestHelpers::RenderOrFail(*this, TEXT("camera"), CameraText))
    {
        return false;
    }
    TestFalse(TEXT("camera page no longer promises the stamp on every path"),
        CameraText.Contains(ForbiddenClaim, ESearchCase::CaseSensitive));
    // Contrast case: the paragraph itself must survive. Deleting the whole alpha-histogram habit
    // would also make the assertion above pass, and that habit is what closed the original
    // multi-day misdiagnosis.
    TestTrue(TEXT("camera page still tells the reader to check the alpha channel"),
        CameraText.Contains(TEXT("alpha channel")));

    FString LevelText;
    if (!WikiDocTestHelpers::RenderOrFail(*this, TEXT("level-building.capture-and-review"), LevelText))
    {
        return false;
    }
    TestFalse(TEXT("level-building capture page no longer promises the stamp on every path"),
        LevelText.Contains(ForbiddenClaim, ESearchCase::CaseSensitive));
    TestTrue(TEXT("level-building capture page still tells the reader to run an alpha histogram"),
        LevelText.Contains(TEXT("alpha histogram")));
    return true;
}

// ============================================================================
// The ortho-path view-mode split, and the REASON the editor debug families are refused.
//
// Two distinct doc defects are pinned here. (1) The shared PINWRIGHT_VIEW_MODE_PARAM_DESC macro
// promises two viewport mode slots and a restore, and points at viewport.viewModeOverride --
// none of which is true on render.capture_ortho_tiles, which has no viewport client and emits no
// viewport block at all. (2) The originating plan justified refusing lightmap density /
// stationary-light overlap / collision with an "editor-scope show-flag override", which is wrong:
// that scope parameter gates exactly ONE line of EngineShowFlagOverride, SetAudioRadius(false),
// and nothing else in the function reads it. The real obstacle is ApplyViewModeOverrides
// early-outing on !AllowDebugViewmodes() and substituting GEngine debug materials per mesh batch.
//
// The distinction is the point: "not implemented, and here is what it would take" is a different
// claim from "cannot be done", and writing the second where the first is true is a failure mode
// this project has paid for repeatedly. These assertions fail if a later edit reverts either page
// to the structurally-impossible framing, or drops the named obstacle for a vague one.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOrthoViewModeSplitDocTest,
    "PinWright.infra.wiki_handler.TopicPage.OrthoViewModeSplit",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FOrthoViewModeSplitDocTest::RunTest(const FString& Parameters)
{
    FString Modes;
    if (!WikiDocTestHelpers::RenderOrFail(*this, TEXT("render.view-modes"), Modes))
    {
        return false;
    }

    // (1) The split: where the ortho verb's verdict is reported, and that it fakes no restore.
    TestTrue(TEXT("view-modes page names showFlags.viewMode as the ortho verb's report location"),
        Modes.Contains(TEXT("showFlags.viewMode")));
    TestTrue(TEXT("view-modes page states the ortho verb omits restored rather than faking it"),
        Modes.Contains(TEXT("omitted rather than faked")));

    // (2) The two refusal families are distinguished, and the real obstacle is named for the
    // one that is a scope decision rather than an impossibility.
    TestTrue(TEXT("view-modes page marks the sub-visualisation family structurally unreachable"),
        Modes.Contains(TEXT("structurally unreachable")));
    TestTrue(TEXT("view-modes page names ApplyViewModeOverrides as the real editor-debug obstacle"),
        Modes.Contains(TEXT("ApplyViewModeOverrides")) && Modes.Contains(TEXT("AllowDebugViewmodes")));
    // The specific correction: the scope parameter gates only SetAudioRadius. Naming it is what
    // stops the refuted reason being re-derived by the next reader.
    TestTrue(TEXT("view-modes page records that the show-flag init scope gates only SetAudioRadius"),
        Modes.Contains(TEXT("SetAudioRadius")));
    TestTrue(TEXT("view-modes page keeps the not-done-versus-cannot-be-done distinction explicit"),
        Modes.Contains(TEXT("not done, and here is what it would take")));

    // The same two facts on the subjects page, which carries the per-family table a caller
    // deciding what to ask for actually reads.
    FString Subjects;
    if (!WikiDocTestHelpers::RenderOrFail(*this, TEXT("render.capture-subjects"), Subjects))
    {
        return false;
    }
    TestTrue(TEXT("capture-subjects table names ApplyViewModeOverrides, not an editor-scope override"),
        Subjects.Contains(TEXT("ApplyViewModeOverrides")) && Subjects.Contains(TEXT("SetAudioRadius")));
    TestTrue(TEXT("capture-subjects table separates structurally unreachable from not implemented"),
        Subjects.Contains(TEXT("structurally unreachable")));
    return true;
}
