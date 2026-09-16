// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for E-editor-camera-readback-not-discoverable-from-focus-actor:
// the editor.* camera-moving verbs (focus_actor / set_camera / jump_to_bookmark)
// and editor.status now point at the camera read-back, which lives in a different
// namespace — `system.inspect.get_viewport_info` (returns cameraLocation /
// cameraRotation / fov). The capability shipped via E-viewport-info-camera-transform
// (EnvironmentHandler.cpp), but no `editor.*` page advertised it, so an agent that
// framed an actor and wanted the resulting pose had to grep the wiki (or, per the
// ticket's #2 evidence, gave up assuming the read-back did not exist).
//
// The fix is docs-only: the `### editor.focus_actor` / `### editor.set_camera` /
// `### editor.jump_to_bookmark` / `### editor.status` per-method sections in
// docs/wiki-src/editor.md now name system.inspect.get_viewport_info as the read
// partner, and the `### system.inspect.get_viewport_info` section in
// docs/wiki-src/system.inspect.md closes the loop from the other side.
//
// These render the live WikiHandler::RenderPage path (the same entry the HTTP
// gateway uses for per-method doc requests), not a copy of the overlay text. The
// pointer text asserted below lives only inside the per-method H3 sections — H3
// sections surface ONLY when an agent calls call("namespace.method") directly, not
// on the namespace page — so reverting the overlay edits makes the renderer omit
// the pointer and every assertion here fails.
#include "Misc/AutomationTest.h"
#include "Tests/Infra/WikiDocTestHelpers.h"

// The per-page assert helper AssertNamesViewportInfoReadback lives beside the other
// wiki-doc regression helpers in Tests/Infra/WikiDocTestHelpers.h (named namespace,
// inline) so any future doc test wanting the same "names system.inspect.get_viewport_info"
// assertion reuses it rather than re-copying.

// ============================================================================
// editor.focus_actor — the existing H3 section (authored by the focus-actor
// identifier-acceptance ticket) is augmented with the read-back pointer; it must
// not have been replaced by a duplicate H3 or lost its identifier-resolution text.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEditorFocusActorCameraReadbackDocTest,
    "PinWright.infra.wiki_handler.Method.EditorFocusActorCameraReadback",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEditorFocusActorCameraReadbackDocTest::RunTest(const FString& Parameters)
{
    FString Text;
    if (!WikiDocTestHelpers::RenderOrFail(*this, TEXT("editor.focus_actor"), Text))
    {
        return false;
    }
    WikiDocTestHelpers::AssertNamesViewportInfoReadback(*this, TEXT("editor.focus_actor"), Text);
    // The augment must preserve the pre-existing identifier-resolution coverage
    // (label / internal name / object path) rather than replace the section.
    TestTrue(TEXT("editor.focus_actor page still documents identifier resolution"),
        Text.Contains(TEXT("object path")) && Text.Contains(TEXT("ACTOR_NOT_FOUND")));
    return true;
}

// ============================================================================
// editor.set_camera — the symmetric setter gets its own H3 naming the read partner.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEditorSetCameraReadbackDocTest,
    "PinWright.infra.wiki_handler.Method.EditorSetCameraReadback",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEditorSetCameraReadbackDocTest::RunTest(const FString& Parameters)
{
    FString Text;
    if (!WikiDocTestHelpers::RenderOrFail(*this, TEXT("editor.set_camera"), Text))
    {
        return false;
    }
    WikiDocTestHelpers::AssertNamesViewportInfoReadback(*this, TEXT("editor.set_camera"), Text);
    return true;
}

// ============================================================================
// editor.jump_to_bookmark — the third camera-moving verb (per the ticket's #2
// evidence) gets the same pointer so a caller can confirm the restored pose.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEditorJumpToBookmarkReadbackDocTest,
    "PinWright.infra.wiki_handler.Method.EditorJumpToBookmarkReadback",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEditorJumpToBookmarkReadbackDocTest::RunTest(const FString& Parameters)
{
    FString Text;
    if (!WikiDocTestHelpers::RenderOrFail(*this, TEXT("editor.jump_to_bookmark"), Text))
    {
        return false;
    }
    WikiDocTestHelpers::AssertNamesViewportInfoReadback(*this, TEXT("editor.jump_to_bookmark"), Text);
    return true;
}

// ============================================================================
// editor.status — the obvious "current editor state" verb must say it does NOT
// carry the camera and route the reader to system.inspect.get_viewport_info.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEditorStatusCameraReadbackDocTest,
    "PinWright.infra.wiki_handler.Method.EditorStatusCameraReadback",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEditorStatusCameraReadbackDocTest::RunTest(const FString& Parameters)
{
    FString Text;
    if (!WikiDocTestHelpers::RenderOrFail(*this, TEXT("editor.status"), Text))
    {
        return false;
    }
    // editor.status carries no camera field of its own, so only the read-back pointer is
    // required (bExpectCameraFields=false), not a cameraLocation/cameraRotation mention.
    WikiDocTestHelpers::AssertNamesViewportInfoReadback(*this, TEXT("editor.status"), Text, /*bExpectCameraFields=*/false);
    return true;
}

// ============================================================================
// system.inspect.get_viewport_info — the reverse pointer: an agent landing here
// first must see it is the read partner for the editor.* camera-moving verbs.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FViewportInfoReadbackPartnerDocTest,
    "PinWright.infra.wiki_handler.Method.ViewportInfoReadbackPartner",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FViewportInfoReadbackPartnerDocTest::RunTest(const FString& Parameters)
{
    FString Text;
    if (!WikiDocTestHelpers::RenderOrFail(*this, TEXT("system.inspect.get_viewport_info"), Text))
    {
        return false;
    }
    TestTrue(TEXT("get_viewport_info page names the editor.* camera-moving verbs it reads back for"),
        Text.Contains(TEXT("editor.focus_actor")) && Text.Contains(TEXT("editor.set_camera")));
    TestTrue(TEXT("get_viewport_info page documents the camera fields it returns"),
        Text.Contains(TEXT("cameraLocation")) && Text.Contains(TEXT("cameraRotation")));
    return true;
}
