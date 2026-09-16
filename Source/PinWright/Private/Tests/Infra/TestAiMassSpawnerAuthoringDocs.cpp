// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for E-mass-spawner-wiki-no-in-mcp-recipe:
// The `ai` namespace prelude advertised "Mass entity configuration" as a flat
// capability, but no page documented how to wire a Mass spawner, and the obvious
// verb `ai.add_mass_spawner` is a phantom-success no-op with a wrong component
// framing (AIHandler.cpp:2135-2184 only marks the Blueprint dirty + saves then
// echoes the request params; UE 5.7 Mass has no UMassSpawnerComponent — a spawner
// is the AMassSpawner actor). The fix is a docs/wiki-src/ai.md overlay: (1) narrow
// the prelude claim to "Mass entity config assets"; (2) add a namespace-page-visible
// `## Mass spawner authoring` section documenting the real in-MCP route (reparent to
// AMassSpawner /Script/MassSpawner.MassSpawner -> blueprint.set_default Count ->
// property.set EntityTypes[0].EntityConfig on the CDO -> property.get verify); and
// (3) a point-of-use `### ai.add_mass_spawner` H3 steering the dead verb to that route.
//
// Both assertions render through the live WikiHandler::RenderPage path (the same
// entry the HTTP gateway serves doc requests from), not a copy of the overlay text.
//
// Overlay-exclusivity (why these markers pin the fix): a grep of the AI handler
// sources finds AMassSpawner / UMassSpawnerComponent / EntityTypes /
// "/Script/MassSpawner.MassSpawner" / blueprint.reparent in ZERO registration
// summaries — they appear only inside the add_mass_spawner runtime `message` string
// (AIHandler.cpp:2178), which never renders into the wiki. So the auto-generated
// `## Methods` index (which sources the registered Summary "Configure a Mass Spawner
// on a blueprint") carries none of them. The `### ai.add_mass_spawner` H3 does not
// render on the namespace page and surfaces only when the method page is rendered
// directly. Reverting any part of the overlay drops the corresponding marker and
// fails the matching assertion.
#include "Misc/AutomationTest.h"
#include "Tests/Infra/WikiDocTestHelpers.h"

// ============================================================================
// ai namespace page: the `## Mass spawner authoring` section documents the real
// AMassSpawner-actor CDO route, and the prelude is narrowed to "Mass entity config
// assets" (no longer an unqualified spawner-wiring claim). Both render onto the
// namespace page via FWikiOverlay.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAiMassSpawnerAuthoringDocTest,
    "PinWright.infra.wiki_handler.Namespace.AiDocumentsMassSpawnerAuthoring",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAiMassSpawnerAuthoringDocTest::RunTest(const FString& Parameters)
{
    FString Text;
    if (!WikiDocTestHelpers::RenderOrFail(*this, TEXT("ai"), Text))
    {
        return false;
    }

    // Prelude narrowed to the capability that actually works (config assets), so the
    // root/namespace index no longer over-promises spawner wiring. This exact phrase
    // lives only in the reworded prelude.
    TestTrue(TEXT("ai prelude narrows the Mass claim to config assets"),
        Text.Contains(TEXT("Mass entity config assets")));

    // The core correction: a Mass spawner is the AMassSpawner actor, not a component
    // (there is no UMassSpawnerComponent). Both markers are overlay-exclusive.
    TestTrue(TEXT("ai page names the AMassSpawner actor as the real spawner"),
        Text.Contains(TEXT("AMassSpawner")));
    TestTrue(TEXT("ai page states there is no UMassSpawnerComponent"),
        Text.Contains(TEXT("UMassSpawnerComponent")));

    // The working in-MCP recipe: reparent target path + the CDO EntityTypes property.
    TestTrue(TEXT("ai page gives the /Script/MassSpawner.MassSpawner reparent target"),
        Text.Contains(TEXT("/Script/MassSpawner.MassSpawner")));
    TestTrue(TEXT("ai page documents the EntityTypes CDO property to set the config"),
        Text.Contains(TEXT("EntityTypes")));
    return true;
}

// ============================================================================
// ai.add_mass_spawner method page: the `### ai.add_mass_spawner` H3 (surfaced only
// via WikiOverlay::LoadMethodSection on the method page) warns the verb wires nothing
// and redirects to the AMassSpawner CDO route. None of these markers are in the
// registered summary or param descriptions.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAiAddMassSpawnerRedirectDocTest,
    "PinWright.infra.wiki_handler.MethodPage.AddMassSpawnerRedirect",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAiAddMassSpawnerRedirectDocTest::RunTest(const FString& Parameters)
{
    FString Text;
    if (!WikiDocTestHelpers::RenderOrFail(*this, TEXT("ai.add_mass_spawner"), Text))
    {
        return false;
    }

    // The honesty steer: the verb does not wire a spawner. Overlay-exclusive prose
    // (the summary is the affirmative "Configure a Mass Spawner on a blueprint").
    TestTrue(TEXT("add_mass_spawner page states the verb does not wire a working spawner"),
        Text.Contains(TEXT("does **not** wire")));

    // The redirect to the real route: the AMassSpawner actor + blueprint.reparent +
    // the EntityTypes CDO property. All overlay-exclusive on this method page.
    TestTrue(TEXT("add_mass_spawner page redirects to the AMassSpawner actor route"),
        Text.Contains(TEXT("AMassSpawner")));
    TestTrue(TEXT("add_mass_spawner page names blueprint.reparent as the first step"),
        Text.Contains(TEXT("blueprint.reparent")));
    TestTrue(TEXT("add_mass_spawner page carries the EntityTypes CDO recipe"),
        Text.Contains(TEXT("EntityTypes")));
    return true;
}
