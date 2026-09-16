// Copyright (c) 2026 Alexander Penkin. MIT License.

// Doc-sync guard for the StaticMesh <-> DynamicMesh round trip.
//
// The registered param glosses already render "overwrite", "reuseExisting" and "assetPath",
// so Text.Contains on those alone passes with the docs/wiki-src/geometry.md overlay reverted.
// These assertions pin only overlay-exclusive facts a caller cannot get from the param table:
// that the in-place path is what PRESERVES materials (the whole reason the flag exists), that
// dynamic meshes persist across a level save (so an agent does not bake purely out of fear of
// losing work), and that reuseExisting deliberately departs from the create_* family.
#include "Misc/AutomationTest.h"
#include "Tests/Infra/WikiDocTestHelpers.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryRoundTripNamespaceDocTest,
    "PinWright.infra.wiki_handler.NamespacePage.GeometryRoundTrip",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGeometryRoundTripNamespaceDocTest::RunTest(const FString& Parameters)
{
    FString Text;
    if (!WikiDocTestHelpers::RenderOrFail(*this, TEXT("geometry"), Text))
    {
        return false;
    }

    // The round trip is named as a pair on the namespace page, not buried in one method page.
    TestTrue(TEXT("geometry page names the load verb"),
        Text.Contains(TEXT("geometry.create_from_static_mesh")));

    // The persistence fact - overlay-only, and the single most load-bearing thing an agent
    // needs to know before deciding whether to bake.
    TestTrue(TEXT("geometry page states dynamic meshes survive a level save"),
        Text.Contains(TEXT("survives level save")) || Text.Contains(TEXT("Dynamic meshes persist")));
    // Inverted deliberately when the geometry verbs gained their own dirty ceremony: the page
    // used to WARN that a mesh edit left the level clean (so the edit could be lost on close).
    // Every mutating/spawning geometry verb now Modify()s the actor and dirties the package, so
    // the page must state that instead. Asserting the old warning would pin a doc claim that is
    // now false; asserting the new statement keeps the same doc surface under test.
    TestTrue(TEXT("geometry page states mutating verbs mark the level dirty"),
        Text.Contains(TEXT("marks the level dirty")));

    // The duplicate trap: editor copy/paste of a >200k-tri dynamic mesh silently substitutes a
    // placeholder cube. Overlay-only, and the most expensive thing on this page to learn the
    // hard way.
    TestTrue(TEXT("geometry page warns about duplicating a large dynamic mesh"),
        Text.Contains(TEXT("actor.duplicate")) && Text.Contains(TEXT("placeholder")));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryCreateFromStaticMeshDocTest,
    "PinWright.infra.wiki_handler.MethodPage.GeometryCreateFromStaticMesh",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGeometryCreateFromStaticMeshDocTest::RunTest(const FString& Parameters)
{
    FString Text;
    if (!WikiDocTestHelpers::RenderOrFail(*this, TEXT("geometry.create_from_static_mesh"), Text))
    {
        return false;
    }

    // The deliberate departure from the create_* family.
    TestTrue(TEXT("page states reuseExisting departs from the create_* verbs"),
        Text.Contains(TEXT("unlike every")) && Text.Contains(TEXT("duplicate actor labels")));

    // The local-space convention, which is what keeps the boolean verbs working.
    TestTrue(TEXT("page states the mesh stays local-space"),
        Text.Contains(TEXT("LOCAL-space")) || Text.Contains(TEXT("not baked into the vertices")));

    // Honest limits.
    TestTrue(TEXT("page states lodIndex is silently clamped"),
        Text.Contains(TEXT("silently clamped")));
    TestTrue(TEXT("page documents lodType as the exact string enum"),
        Text.Contains(TEXT("`lodType` is a **string enum**: `MaxAvailable` (default) | `HiResSourceModel` | `SourceModel` | `RenderData`")));
    TestTrue(TEXT("page states ISM/HISM instances are not baked"),
        Text.Contains(TEXT("ISM/HISM")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryConvertOverwriteDocTest,
    "PinWright.infra.wiki_handler.MethodPage.GeometryConvertOverwrite",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGeometryConvertOverwriteDocTest::RunTest(const FString& Parameters)
{
    FString Text;
    if (!WikiDocTestHelpers::RenderOrFail(*this, TEXT("geometry.convert_to_static_mesh"), Text))
    {
        return false;
    }

    // The reason overwrite exists at all.
    TestTrue(TEXT("page states the create path drops materials"),
        Text.Contains(TEXT("materials are NOT carried")));
    TestTrue(TEXT("page states the in-place path preserves them"),
        Text.Contains(TEXT("preserving")) || Text.Contains(TEXT("preserves")));

    // The destructive-opt-in framing, matching static_mesh.bake_transform's wording.
    TestTrue(TEXT("page marks overwrite as in-place and destructive"),
        Text.Contains(TEXT("in-place and destructive")));

    // The Nanite trap.
    TestTrue(TEXT("page states the HiRes source is written too"),
        Text.Contains(TEXT("HiRes")) && Text.Contains(TEXT("revert")));
    return true;
}
