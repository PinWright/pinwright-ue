// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for E-ik-chain-bone-name-discovery:
// Authoring an IK chain via animation.authoring.add_ik_chain requires startBone/endBone
// bone names, but from the animation.authoring workflow nothing pointed callers at the
// bone-name reader (skeleton.list_bones), and that method had no hand-authored overlay
// section. skeleton.list_bones ships (SkeletonHandler.cpp:137-188) but get_animation_info
// on a USkeleton is bare ({assetType:"Skeleton"} only) and asset.get returns no bones, so
// an agent hunting for bone names fell back to asset.dump + an on-disk skeleton.json Read.
//
// The fix is docs-overlay-only on three surfaces:
//   1. docs/wiki-src/skeleton.md gains a `### skeleton.list_bones` H3 documenting the bone
//      enumerator (it previously documented only describe_mesh / describe_skin_weights),
//      naming it the bone-name source for add_ik_chain.
//   2. docs/wiki-src/animation.authoring.md gains a dedicated
//      `### animation.authoring.add_ik_chain` H3 pointing at skeleton.list_bones as the
//      startBone/endBone source. (A dedicated H3 is required: the IK-rig family block is
//      keyed to `### animation.authoring.create_ik_rig`, so
//      WikiOverlay::LoadMethodSection("...add_ik_chain") finds nothing there and the
//      per-method page renders no overlay notes without it.)
//   3. The `## Inspect-after-mutate` section of animation.authoring.md notes the Skeleton
//      branch of get_animation_info is bare and routes to skeleton.list_bones.
//
// All assertions render through the live WikiHandler::RenderPage method-page / namespace-page
// path (the same entry the HTTP gateway uses for doc requests), surfacing the H3 overlay via
// WikiOverlay::LoadMethodSection — not a copy of the overlay text. The markers asserted are
// overlay-exclusive: the auto-generated summaries/param specs name no bone-name source, and
// before this fix LoadMethodSection found no section keyed exactly
// "animation.authoring.add_ik_chain" (the old multi-verb heading keyed a non-method name) nor
// "skeleton.list_bones" at all — so reverting either overlay edit makes LoadMethodSection
// return empty and these assertions fail.
//
// This file carries the union of two hosts' regression tests for the same ticket (merged
// during a concurrent rebase): both the MethodPage.* cases (host A) and the Method.* /
// Namespace.* cases (host B) are kept so neither host's coverage is lost.
#include "Misc/AutomationTest.h"
#include "Tests/Infra/WikiDocTestHelpers.h"

// ============================================================================
// add_ik_chain method page: the dedicated H3 overlay section cross-references
// skeleton.list_bones as the startBone/endBone source. This H3 surfaces only when
// the add_ik_chain method page is rendered directly (LoadMethodSection keys on the
// exact method name) — the namespace page and auto param spec name no bone source.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAddIkChainBoneNameSourceDocTest,
    "PinWright.infra.wiki_handler.MethodPage.AddIkChainBoneNameSource",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAddIkChainBoneNameSourceDocTest::RunTest(const FString& Parameters)
{
    FString Text;
    if (!WikiDocTestHelpers::RenderOrFail(*this, TEXT("animation.authoring.add_ik_chain"), Text))
    {
        return false;
    }

    // The add_ik_chain method page must name skeleton.list_bones as the bone-name source
    // for startBone/endBone — the cross-reference the IK workflow previously lacked. This
    // is the load-bearing contract of the fix; the canonical home of the get_animation_info
    // dead-end rationale is the skeleton.list_bones page (asserted there), so this page
    // only needs to carry the pointer, not restate the dead-end prose.
    TestTrue(TEXT("add_ik_chain page cross-references skeleton.list_bones as the bone-name source"),
        Text.Contains(TEXT("skeleton.list_bones")));
    TestTrue(TEXT("add_ik_chain page ties list_bones to the startBone/endBone params"),
        Text.Contains(TEXT("startBone")) && Text.Contains(TEXT("endBone")));
    return true;
}

// ============================================================================
// skeleton.list_bones method page: the hand-authored overlay section describing the
// return shape and naming it the bone-name source for add_ik_chain. Before the fix
// skeleton.md had no `### skeleton.list_bones` section, so LoadMethodSection returned
// empty and the method page carried only the auto param spec (no parentName/location
// return description, no add_ik_chain back-reference).
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSkeletonListBonesOverlayDocTest,
    "PinWright.infra.wiki_handler.MethodPage.SkeletonListBonesOverlay",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSkeletonListBonesOverlayDocTest::RunTest(const FString& Parameters)
{
    FString Text;
    if (!WikiDocTestHelpers::RenderOrFail(*this, TEXT("skeleton.list_bones"), Text))
    {
        return false;
    }

    // The curated overlay section back-references add_ik_chain as a consumer — this
    // pointer lives only in the hand-authored H3, not in the auto summary.
    TestTrue(TEXT("skeleton.list_bones page names add_ik_chain as a bone-name consumer"),
        Text.Contains(TEXT("add_ik_chain")));
    // The overlay describes the return shape (parentName + ref-pose location), which the
    // auto param spec (params only) cannot supply.
    TestTrue(TEXT("skeleton.list_bones page describes the returned bone fields"),
        Text.Contains(TEXT("parentName")) && Text.Contains(TEXT("location")));
    // This page is the single canonical home for the get_animation_info / asset.get
    // dead-end rationale (the add_ik_chain page only points here), so it must carry the
    // full {assetType:"Skeleton"} explanation.
    TestTrue(TEXT("skeleton.list_bones page notes get_animation_info on a Skeleton returns no bones"),
        Text.Contains(TEXT("get_animation_info")) && Text.Contains(TEXT("assetType")));
    return true;
}

// ============================================================================
// skeleton.list_bones — the per-method H3 must document the bone enumerator: its
// bone fields and that it is the canonical bone-name reader (so an agent hunting
// bone names lands here instead of dumping the Skeleton to disk).
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSkeletonListBonesDiscoveryDocTest,
    "PinWright.infra.wiki_handler.Method.SkeletonListBonesDiscovery",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSkeletonListBonesDiscoveryDocTest::RunTest(const FString& Parameters)
{
    FString Text;
    if (!WikiDocTestHelpers::RenderOrFail(*this, TEXT("skeleton.list_bones"), Text))
    {
        return false;
    }
    // The H3 names the per-bone fields the handler returns (parentName + parentIndex),
    // proving the section documents the actual bone enumerator, not a stub.
    TestTrue(TEXT("skeleton.list_bones page documents the per-bone fields it returns"),
        Text.Contains(TEXT("parentName")) && Text.Contains(TEXT("parentIndex")));
    // It frames list_bones as the bone-name source for add_ik_chain, the discovery
    // path the ticket says was missing.
    TestTrue(TEXT("skeleton.list_bones page points at add_ik_chain as a consumer of bone names"),
        Text.Contains(TEXT("add_ik_chain")));
    return true;
}

// ============================================================================
// animation.authoring.add_ik_chain — the dedicated per-method H3 must point at
// skeleton.list_bones as the startBone/endBone source. Without the dedicated H3
// (the family heading is combined and matches no single method) this page renders
// no overlay notes at all, so this assertion guards both the cross-reference AND
// the dedicated-section requirement, plus the CHAIN_NOT_ADDED next-step framing.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAddIkChainBoneNameSourceMethodDocTest,
    "PinWright.infra.wiki_handler.Method.AddIkChainBoneNameSource",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAddIkChainBoneNameSourceMethodDocTest::RunTest(const FString& Parameters)
{
    FString Text;
    if (!WikiDocTestHelpers::RenderOrFail(*this, TEXT("animation.authoring.add_ik_chain"), Text))
    {
        return false;
    }
    // The per-method page names skeleton.list_bones as the bone-name source.
    TestTrue(TEXT("add_ik_chain page points at skeleton.list_bones for startBone/endBone names"),
        Text.Contains(TEXT("skeleton.list_bones")));
    // It still documents the startBone/endBone requirement and CHAIN_NOT_ADDED so the
    // pointer reads as the obvious next step after a rejection.
    TestTrue(TEXT("add_ik_chain page documents the startBone/endBone requirement and CHAIN_NOT_ADDED"),
        Text.Contains(TEXT("startBone")) && Text.Contains(TEXT("endBone")) && Text.Contains(TEXT("CHAIN_NOT_ADDED")));
    return true;
}

// ============================================================================
// animation.authoring namespace page — the Inspect-after-mutate `##` section must
// note that get_animation_info on a Skeleton is bare and route bone enumeration to
// skeleton.list_bones, so the step-1 dead-end the agent hit redirects in one hop.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAnimAuthoringSkeletonBoneEnumDocTest,
    "PinWright.infra.wiki_handler.Namespace.AnimAuthoringSkeletonBoneEnum",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAnimAuthoringSkeletonBoneEnumDocTest::RunTest(const FString& Parameters)
{
    FString Text;
    if (!WikiDocTestHelpers::RenderOrFail(*this, TEXT("animation.authoring"), Text))
    {
        return false;
    }
    // The namespace page routes Skeleton bone enumeration to skeleton.list_bones, not
    // get_animation_info — the marker lives in the overlay Inspect-after-mutate section.
    TestTrue(TEXT("animation.authoring page routes Skeleton bone enumeration to skeleton.list_bones"),
        Text.Contains(TEXT("skeleton.list_bones")));
    TestTrue(TEXT("animation.authoring page notes get_animation_info on a Skeleton is bare"),
        Text.Contains(TEXT("get_animation_info")) && Text.Contains(TEXT("Skeleton")));
    return true;
}
