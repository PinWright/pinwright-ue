// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for E-animation-sequence-info-reader-name-split: the "read a
// UAnimSequence so I know its length / frame count" intent is split across two
// methods that share neither namespace nor verb — the top-level dump-parity reader
// is `animation.describe_sequence`, while the `get_animation_info` verb lives only
// under the `animation.authoring` sub-namespace. There is no `animation.get_animation_info`,
// so the intuitive composition UNKNOWN_ACTIONs. Before the fix the top-level reader
// was buried mid-sentence in animation.md's "How to use" list with no signpost, and
// animation.authoring.md's Inspect-after-mutate section never cross-referenced the
// top-level reader. The fix adds a `## Read a sequence's length / info` namespace
// section to docs/wiki-src/animation.md naming `animation.describe_sequence` as the
// go-to reader and reconciling it with the authoring-namespaced get_animation_info,
// plus a reciprocal cross-reference in docs/wiki-src/animation.authoring.md's
// Inspect-after-mutate section pointing back at describe_sequence.
//
// These markers live only in the overlay `##` sections of the two pages, which render
// on the namespace page through WikiHandler::RenderPage -> RenderNamespaceHeader ->
// WikiOverlay::LoadGroupSections. This exercises the live render path the HTTP gateway
// uses for doc requests, not a copy of the overlay text. Every marker asserted below is
// overlay-exclusive (the auto Methods index and per-verb auto summaries name no
// describe_sequence-vs-get_animation_info reconciliation and never warn that
// animation.get_animation_info does not exist), so reverting the overlay edits makes
// LoadGroupSections return the original content and these assertions fail.
#include "Misc/AutomationTest.h"
#include "Tests/Infra/WikiDocTestHelpers.h"

// ============================================================================
// animation namespace page: the `## Read a sequence's length / info` section names
// `describe_sequence` as the go-to sequence reader, warns that the intuitive
// `animation.get_animation_info` guess does not exist, and points at the
// authoring-namespaced get_animation_info as the introspection dual.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAnimSequenceInfoReaderTopLevelDocTest,
    "PinWright.infra.wiki_handler.Namespace.AnimSequenceInfoReaderNameSplit",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAnimSequenceInfoReaderTopLevelDocTest::RunTest(const FString& Parameters)
{
    FString Text;
    if (!WikiDocTestHelpers::RenderOrFail(*this, TEXT("animation"), Text))
    {
        return false;
    }

    // The dedicated "read a sequence" signpost section must exist on the namespace page.
    TestTrue(TEXT("animation page carries a 'Read a sequence' length/info section"),
        Text.Contains(TEXT("Read a sequence")));
    // It names describe_sequence as the go-to top-level reader.
    TestTrue(TEXT("animation page names animation.describe_sequence as the go-to sequence reader"),
        Text.Contains(TEXT("describe_sequence")));
    // It warns that the intuitive get_animation_info guess does not resolve at top level.
    TestTrue(TEXT("animation page warns there is no animation.get_animation_info / it UNKNOWN_ACTIONs"),
        Text.Contains(TEXT("animation.get_animation_info")) && Text.Contains(TEXT("UNKNOWN_ACTION")));
    // It reconciles the two info readers, pointing at the authoring-namespaced verb.
    TestTrue(TEXT("animation page points at animation.authoring.get_animation_info as the introspection dual"),
        Text.Contains(TEXT("animation.authoring.get_animation_info")));
    return true;
}

// ============================================================================
// animation.authoring namespace page: the Inspect-after-mutate section now carries
// the reciprocal cross-reference — get_animation_info is sub-namespaced here (no
// top-level animation.get_animation_info), and the full dump-parity read is the
// top-level describe_sequence.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAnimSequenceInfoReaderAuthoringDocTest,
    "PinWright.infra.wiki_handler.Namespace.AnimSequenceInfoReaderNameSplitAuthoring",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAnimSequenceInfoReaderAuthoringDocTest::RunTest(const FString& Parameters)
{
    FString Text;
    if (!WikiDocTestHelpers::RenderOrFail(*this, TEXT("animation.authoring"), Text))
    {
        return false;
    }

    // The reciprocal cross-reference back to the top-level reader must exist.
    TestTrue(TEXT("animation.authoring page cross-references the top-level describe_sequence reader"),
        Text.Contains(TEXT("describe_sequence")));
    // It states get_animation_info is sub-namespaced (no top-level animation.get_animation_info).
    TestTrue(TEXT("animation.authoring page notes there is no top-level animation.get_animation_info"),
        Text.Contains(TEXT("animation.get_animation_info")));
    return true;
}
