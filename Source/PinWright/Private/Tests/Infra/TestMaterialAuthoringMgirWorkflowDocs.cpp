// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for E-material-mgir-bulk-path-undiscovered:
// The material.authoring Workflow used to teach ONLY the imperative drip
// (create -> add_* -> connect_nodes -> compile_material), so an agent building a
// multi-node graph from a brief did the per-node/per-wire chain even though the
// one-call bulk path material.compile_mgir (MGIR text-IR, shipped DONE under
// F-mgir-material-graph-ir) authors every node + wire + layout from one text body.
// MGIR was surfaced only in a trailer of the ### material.authoring.compile_material
// per-method H3 section, which does NOT render on the namespace page (per the wiki
// render rules), so it was invisible at the build-decision point.
//
// The fix adds a one-clause steer to the ## Workflow section of
// docs/wiki-src/material.authoring.md: "for a graph of more than a few nodes,
// prefer one material.compile_mgir text-IR call over the imperative add_* +
// connect_nodes chain ... see material.mgir."
//
// This exercises the live WikiHandler::RenderPage render path (the same entry the
// HTTP gateway uses for doc requests), not a copy of the overlay text. The ##
// Workflow section renders on the material.authoring namespace page.
//
// Each assertion below is keyed to a marker that the new Workflow clause is the
// ONLY source of on the rendered material.authoring page, so reverting the clause
// fails it:
//  - `material.compile_mgir` is registered under the `material` namespace, not
//    `material.authoring`, so it is absent from this page's auto-generated
//    ## Methods index and from the baseline body.
//  - the steer phrases `more than a few nodes` / `multi-node graph from scratch`
//    are overlay-exclusive prose.
//  - the link target `material.mgir.md` does not occur in the baseline (the
//    baseline only mentions `material.decompile_mgir`).
// Markers that are NOT overlay-exclusive here (and so are deliberately not the
// load-bearing check): `connect_nodes` (a registered material.authoring method,
// also named in the baseline ## Limitations section) and the bare `material.mgir`
// substring (contained in the baseline's `material.decompile_mgir`).
#include "Misc/AutomationTest.h"
#include "Tests/Infra/WikiDocTestHelpers.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialAuthoringMgirWorkflowDocTest,
    "PinWright.infra.wiki_handler.Namespace.MaterialAuthoringMgirWorkflow",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMaterialAuthoringMgirWorkflowDocTest::RunTest(const FString& Parameters)
{
    FString Text;
    if (!WikiDocTestHelpers::RenderOrFail(*this, TEXT("material.authoring"), Text))
    {
        return false;
    }

    // (1) The MGIR bulk path is surfaced at the build-decision point — i.e. the
    // Workflow section names material.compile_mgir, not just the deep per-method
    // compile_material trailer (which does not render on the namespace page).
    TestTrue(TEXT("material.authoring namespace page surfaces material.compile_mgir"),
        Text.Contains(TEXT("material.compile_mgir")));

    // (2) It steers multi-node from-scratch authoring to the one-call bulk path
    // over the imperative add_* + connect_nodes chain. The load-bearing marker is
    // the steer phrase, which is overlay-exclusive; `connect_nodes` is NOT — it is a
    // registered material.authoring method (so it appears in the auto-generated
    // ## Methods index) and also occurs in the baseline ## Limitations section, so
    // an AND with it would guard nothing. Assert only the overlay-exclusive phrase.
    TestTrue(TEXT("Workflow steers large graphs to the bulk MGIR call over the imperative chain"),
        Text.Contains(TEXT("more than a few nodes")) || Text.Contains(TEXT("multi-node graph from scratch")));

    // (3) It links the material.mgir topic page so the syntax is one hop away.
    // Assert on the markdown link target (`material.mgir.md`), not the bare
    // `material.mgir` substring: the baseline ## Limitations section already
    // mentions `material.decompile_mgir` (which contains "material.mgir"), so a
    // bare-substring check would pass even with the Workflow clause reverted. The
    // `.md` link target appears nowhere in the baseline, so this is overlay-exclusive.
    TestTrue(TEXT("Workflow links the material.mgir topic page"),
        Text.Contains(TEXT("material.mgir.md")));

    return true;
}
