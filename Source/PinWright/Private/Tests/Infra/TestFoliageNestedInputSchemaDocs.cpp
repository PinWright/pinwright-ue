// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for E-foliage-nested-input-schemas-undocumented:
// foliage.add_instances and foliage.create_procedural register their param
// *names* (transforms/locations, bounds/foliageTypes) with one-line glosses
// ("Array of {location, rotation, scale} transforms", "Volume bounds with
// location and size", "Array of foliage type configs with meshPath and
// density") but never the nested value shapes an author must fill in
// (FoliageHandler.cpp registrations at add_instances :610-615 /
// create_procedural :833-839). The real schema lived only in the handler C++:
// transforms[].location is required and an entry without it was silently dropped
// (:682 - it is now reported in the response's skipped[] array), scale accepts a
// uniformScale scalar alias (:721), bounds.size accepts
// an array form (:876-882), and foliageTypes[] entries carry their own nested keys.
// (Which keys those are has since changed: B-create-procedural-ignores-scale-and-normal-fields
// made minScale/maxScale/alignToNormal honoured rather than dropped, leaving randomYaw
// as the one add_type key create_procedural still ignores.)
//
// The fix adds `### foliage.add_instances` and `### foliage.create_procedural`
// H3 overlay sections to docs/wiki-src/foliage.md documenting those nested
// shapes and the two non-obvious rules (silent-drop without location; which
// foliageTypes[] keys are read). This test renders the live method pages
// through WikiHandler::RenderPage (the same entry the HTTP gateway uses for doc
// requests), not a copy of the overlay text. Each marker asserted below is
// chosen to be overlay-exclusive: the bare param-name words (location, rotation,
// scale, size, meshPath, density) ARE already rendered from the auto-generated
// param descriptions, so this test does NOT assert on them — it asserts only on
// distinctive overlay phrasing the gloss lacks (the silent-drop rule, the
// uniformScale-is-a-sibling-key note, the transforms-wins precedence rule,
// bounds.size's {1000,1000,1000} default, and the per-entry scale/align rules).
// Reverting the overlay makes LoadMethodSection return empty and these fail.
#include "Misc/AutomationTest.h"
#include "Tests/Infra/WikiDocTestHelpers.h"

// ============================================================================
// Method page: `### foliage.add_instances` H3 documents the transforms nested
// shape (location required + silently dropped if absent; rotation/scale optional;
// scale's uniformScale scalar alias) and the legacy `locations` fallback.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFoliageAddInstancesSchemaDocTest,
    "PinWright.infra.wiki_handler.MethodPage.FoliageAddInstancesSchema",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FFoliageAddInstancesSchemaDocTest::RunTest(const FString& Parameters)
{
    FString Text;
    if (!WikiDocTestHelpers::RenderOrFail(*this, TEXT("foliage.add_instances"), Text))
    {
        return false;
    }

    // The bare keys "location"/"rotation"/"scale"/"locations" are NOT
    // overlay-exclusive: the param glosses ("Array of {location, rotation,
    // scale} transforms", the literal `locations` param name) already render
    // them, so Text.Contains on those words passes with the overlay reverted.
    // Assert instead on distinctive overlay-only phrasing the gloss lacks.

    // The non-obvious drop rule: a transform without a valid location places nothing.
    // The drop is now reported (skipped[] + skippedCount) rather than silent, and the
    // overlay must name the readback fields so a caller can detect it.
    TestTrue(TEXT("add_instances page states an entry without location places nothing and is reported"),
        Text.Contains(TEXT("skipped[]")) && Text.Contains(TEXT("skippedCount")));

    // The uniformScale scalar alias — present in the handler (:721) but absent
    // from the param gloss; the overlay states it's a sibling key on the entry.
    TestTrue(TEXT("add_instances page documents uniformScale as a key on the transform entry"),
        Text.Contains(TEXT("uniformScale")) && Text.Contains(TEXT("not inside")));

    // The legacy `locations` precedence rule: ignored when transforms is present
    // ("transforms wins") — overlay-only; the param gloss only names the field.
    TestTrue(TEXT("add_instances page documents the legacy locations fallback precedence"),
        Text.Contains(TEXT("transforms")) && Text.Contains(TEXT("wins")));
    return true;
}

// ============================================================================
// Method page: `### foliage.create_procedural` H3 documents the bounds nested
// shape (location + size, size's array form), how foliageTypes[] applies per-entry
// minScale/maxScale/alignToNormal (and that randomYaw alone is still unread), and the
// tiling grid the response echoes back.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFoliageCreateProceduralSchemaDocTest,
    "PinWright.infra.wiki_handler.MethodPage.FoliageCreateProceduralSchema",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FFoliageCreateProceduralSchemaDocTest::RunTest(const FString& Parameters)
{
    FString Text;
    if (!WikiDocTestHelpers::RenderOrFail(*this, TEXT("foliage.create_procedural"), Text))
    {
        return false;
    }

    // The bare keys "location"/"size"/"meshPath"/"density" are NOT
    // overlay-exclusive: the param glosses ("Volume bounds with location and
    // size", "Array of foliage type configs with meshPath and density") already
    // render them, so Text.Contains on those words passes with the overlay
    // reverted. Assert instead on distinctive overlay-only phrasing.

    // bounds.size accepts an array form and has a concrete default — overlay-only
    // (the param desc says only "Volume bounds with location and size").
    TestTrue(TEXT("create_procedural page documents bounds.size array form + default"),
        Text.Contains(TEXT("{1000,1000,1000}")));

    // The asymmetry caveat has moved, so this assertion moved with it:
    // B-create-procedural-ignores-scale-and-normal-fields made the verb HONOUR
    // per-entry minScale/maxScale/alignToNormal, and the overlay now documents how
    // they are applied plus the one add_type key still unread (randomYaw). The bare
    // words "minScale"/"alignToNormal" stopped being overlay-exclusive when the
    // foliageTypes param gloss started naming them, so the markers here are the
    // overlay-only phrasing: the uniform-interval application rule and randomYaw.
    TestTrue(TEXT("create_procedural page states how per-entry minScale/maxScale apply and which key is still unread"),
        Text.Contains(TEXT("uniform interval")) && Text.Contains(TEXT("randomYaw")));

    // The tiling grid the simulation runs on. `tileSize`/`numUniqueTiles` are param
    // names and so render from the registration; the response field names are
    // overlay-only, and they are what proves the effective values are observable.
    TestTrue(TEXT("create_procedural page documents the echoed tiling fields"),
        Text.Contains(TEXT("num_unique_tiles")));

    // The simulation half of foliageTypes[], added by
    // F-procedural-foliage-simulation-knobs-unreachable /
    // B-create-procedural-density-writes-paint-density. The key NAMES render from the
    // foliageTypes param gloss, so they are not overlay-exclusive; these two are:
    // `initial_seed_density_source` is a response field the registration never names, and
    // `spreadVariance` is one of the properties still unreachable through the verb, which
    // only the overlay's "still unreachable" list mentions.
    TestTrue(TEXT("create_procedural page documents the per-type simulation echo and what is still unreachable"),
        Text.Contains(TEXT("initial_seed_density_source")) && Text.Contains(TEXT("spreadVariance")));
    return true;
}
