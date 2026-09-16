// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for E-anim-blueprint-create-two-methods-discovery: two
// near-identically-named methods create a UAnimBlueprint — the top-level
// `animation.create_animation_bp` (AnimationHandler.cpp), meshPath/skeletonPath
// capable and routed through the safe FAssetToolsModule::CreateAsset de-dupe path,
// plus the authoring `animation.authoring.create_anim_blueprint`
// (AnimationAuthoringHandler_AnimBlueprint.cpp), which is skeletonPath-only and
// rejects an in-session name collision with a guarded ASSET_EXISTS. (A third
// sibling, `animation.create_anim_blueprint`, was removed in the RPC cull.) The
// registry method summaries already cross-reference the siblings, but the
// hand-authored overlay prose never disambiguated which creator to pick. The fix
// adds a `## Which anim-BP creator?` section to docs/wiki-src/animation.md (naming
// all three creators, the meshPath auto-resolve, and the safe-CreateAsset vs
// authoring-FactoryCreateNew split) and a reciprocal `## Which anim-BP creator?`
// section to docs/wiki-src/animation.authoring.md (authoring verb is skeletonPath-only
// and returns ASSET_EXISTS, cross-linking the top-level meshPath-capable creator).
//
// These markers live only in the overlay `##` sections of the two pages, which render
// on the namespace page through WikiHandler::RenderPage -> RenderNamespaceHeader ->
// WikiOverlay::LoadGroupSections. This exercises the live render path the HTTP gateway
// uses for doc requests, not a copy of the overlay text.
//
// Every assertion below targets a phrase that is overlay-EXCLUSIVE — absent from the auto
// Methods index and per-verb auto summaries. That distinction is load-bearing: the registry
// summaries already echo the method names, the word "meshPath" (incl. create_control_rig's
// "skeletalMeshPath"), and the cross-references between the sibling creators, so asserting
// those strings would pass against auto content even if the hand-authored guidance were
// stripped. The `Which anim-BP creator` heading and the `ASSET_EXISTS` guard wording are the
// full-revert guards (a section deletion drops them); the guidance phrases ("auto-resolves
// the Skeleton", "one-shot creator", "no `meshPath`") guard against
// a *partial* regression of the disambiguation prose this section exists to protect.
#include "Misc/AutomationTest.h"
#include "Tests/Infra/WikiDocTestHelpers.h"

// ============================================================================
// animation namespace page: the `## Which anim-BP creator?` section names both
// creators, surfaces the meshPath auto-resolve, and cross-links the authoring peer.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAnimBlueprintCreatorTopLevelDocTest,
    "PinWright.infra.wiki_handler.Namespace.AnimBlueprintCreatorDisambiguation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAnimBlueprintCreatorTopLevelDocTest::RunTest(const FString& Parameters)
{
    FString Text;
    if (!WikiDocTestHelpers::RenderOrFail(*this, TEXT("animation"), Text))
    {
        return false;
    }

    // The dedicated "which creator?" disambiguation section must exist on the page
    // (overlay-exclusive heading — the auto Methods index never emits it). This is the
    // full-revert guard.
    TestTrue(TEXT("animation page carries a 'Which anim-BP creator' disambiguation section"),
        Text.Contains(TEXT("Which anim-BP creator")));
    // The guidance assertions match phrases unique to the hand-authored overlay prose, not
    // method names / "meshPath" that the auto Methods index already echoes (the registry
    // summaries cross-reference the siblings and mention meshPath, so asserting those would
    // pass even if the guidance were stripped). These catch a *partial* regression of the
    // disambiguation guidance, not just a full section deletion.
    //
    // create_animation_bp is characterized as the recommended one-shot creator whose meshPath
    // auto-resolves the Skeleton — the disambiguating capability.
    TestTrue(TEXT("animation page recommends create_animation_bp as the one-shot creator"),
        Text.Contains(TEXT("one-shot creator")));
    TestTrue(TEXT("animation page documents the meshPath skeleton auto-resolve as the disambiguator"),
        Text.Contains(TEXT("auto-resolves the Skeleton")));
    // It records the authoring peer's guarded ASSET_EXISTS rejection (overlay-exclusive;
    // softened from the stale "crashes the editor" claim). Second full-revert guard.
    TestTrue(TEXT("animation page notes the authoring peer's guarded ASSET_EXISTS rejection"),
        Text.Contains(TEXT("ASSET_EXISTS")));
    return true;
}

// ============================================================================
// animation.authoring namespace page: the reciprocal `## Which anim-BP creator?`
// section says the authoring verb is skeletonPath-only and returns ASSET_EXISTS, and
// cross-links the top-level meshPath-capable creator.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAnimBlueprintCreatorAuthoringDocTest,
    "PinWright.infra.wiki_handler.Namespace.AnimBlueprintCreatorDisambiguationAuthoring",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAnimBlueprintCreatorAuthoringDocTest::RunTest(const FString& Parameters)
{
    FString Text;
    if (!WikiDocTestHelpers::RenderOrFail(*this, TEXT("animation.authoring"), Text))
    {
        return false;
    }

    // The reciprocal disambiguation section must exist (overlay-exclusive heading).
    // Full-revert guard.
    TestTrue(TEXT("animation.authoring page carries a 'Which anim-BP creator' disambiguation section"),
        Text.Contains(TEXT("Which anim-BP creator")));
    // Guidance assertions match overlay-exclusive prose, not the method name / "meshPath" the
    // auto Methods index echoes (the authoring summary already says "Largely overlaps with
    // animation.create_animation_bp" and create_control_rig's summary contains "skeletalMeshPath",
    // so bare-name / "meshPath" asserts would pass against auto content regardless of the prose).
    //
    // The authoring verb is skeletonPath-only with no meshPath auto-resolve.
    TestTrue(TEXT("animation.authoring page states the verb has no meshPath auto-resolve"),
        Text.Contains(TEXT("no `meshPath`")));
    // It cross-links the meshPath-capable one-shot creator on the top-level page.
    TestTrue(TEXT("animation.authoring page cross-links the top-level one-shot creator"),
        Text.Contains(TEXT("one-shot creator")));
    // It states the authoring verb returns a guarded ASSET_EXISTS (not a crash). Second
    // full-revert guard.
    TestTrue(TEXT("animation.authoring page notes the guarded ASSET_EXISTS rejection"),
        Text.Contains(TEXT("ASSET_EXISTS")));
    return true;
}
