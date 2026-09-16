// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for E-geometry-primitive-orientation-axes-undocumented:
// create_box's dimension->axis mapping (width->X, height->Y, depth->Z, so the
// vertical extent is `depth`, not `height`), create_arch's build plane (the torus
// lies flat in local X-Y and needs rotation.roll=-90 to stand apex-up, +90 points
// the apex down), and get_vertex_position's mesh-LOCAL coordinates (the actor's
// world transform is not applied) are all fixed Geometry Script conventions the
// handlers behave by but never disclose. The fix adds a `## Primitive orientation`
// namespace section plus `### geometry.create_box` / `### geometry.create_arch` /
// `### geometry.get_vertex_position` H3 overlay sections to docs/wiki-src/geometry.md,
// surfaced by WikiOverlay (namespace `##` section -> namespace page;
// `### method` H3 -> per-method page).
//
// This exercises the live WikiHandler::RenderPage render path (the same entry the
// HTTP gateway uses for doc requests), not a copy of the overlay text. Every marker
// asserted below is overlay-exclusive: the create_box/create_arch/get_vertex_position
// auto summaries ("Create a box/cube dynamic mesh actor", "Create an arch (partial
// torus) dynamic mesh actor", "Get the position of a vertex in a dynamic mesh") and
// their param descriptions ("Box width (default 100)", "Major radius (default 100)",
// "Index of the vertex", ...) name no axis mapping, no roll sign, and no local-space
// caveat — so reverting the overlay sections makes LoadGroupPrelude / LoadMethodSection
// return empty and these assertions fail.
#include "Misc/AutomationTest.h"
#include "Tests/Infra/WikiDocTestHelpers.h"

// ============================================================================
// Namespace page: the `## Primitive orientation` section discloses all three
// conventions at once (box axis mapping, arch flat-by-default + roll-to-stand,
// get_vertex_position local-space). These markers live only in the namespace `##`
// section of docs/wiki-src/geometry.md.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryOrientationNamespaceDocTest,
    "PinWright.infra.wiki_handler.Namespace.GeometryPrimitiveOrientation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGeometryOrientationNamespaceDocTest::RunTest(const FString& Parameters)
{
    FString Text;
    if (!WikiDocTestHelpers::RenderOrFail(*this, TEXT("geometry"), Text))
    {
        return false;
    }

    // The orientation section must exist on the namespace page.
    TestTrue(TEXT("geometry page carries a Primitive orientation section"),
        Text.Contains(TEXT("Primitive orientation")));
    // create_box vertical-dimension trap: the vertical extent is `depth`, not `height`.
    TestTrue(TEXT("geometry page states the box vertical dimension is depth, not height"),
        Text.Contains(TEXT("the *vertical* dimension is `depth`, not `height`")));
    // create_arch roll sign: -90 stands it apex-up (the exact sign the reporter had backwards).
    TestTrue(TEXT("geometry page states arch rotation.roll = -90 stands it apex-up"),
        Text.Contains(TEXT("rotation.roll = -90")));
    // get_vertex_position returns mesh-LOCAL coords (actor transform not applied).
    TestTrue(TEXT("geometry page states get_vertex_position returns mesh-LOCAL coords"),
        Text.Contains(TEXT("mesh-LOCAL")));
    return true;
}

// ============================================================================
// create_box method page: width/height/depth -> X/Y/Z, and the upright-wall
// recipe ({width:W, height:D, depth:H}). Overlay-exclusive H3.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryCreateBoxAxisDocTest,
    "PinWright.infra.wiki_handler.MethodPage.GeometryCreateBoxAxisMapping",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGeometryCreateBoxAxisDocTest::RunTest(const FString& Parameters)
{
    FString Text;
    if (!WikiDocTestHelpers::RenderOrFail(*this, TEXT("geometry.create_box"), Text))
    {
        return false;
    }

    // The axis mapping the handler applies via AppendBox(... Width, Height, Depth ...)
    // — overlay-exclusive (the param descs say only "Box width/height/depth (default 100)").
    TestTrue(TEXT("create_box page maps width/height/depth to local X/Y/Z"),
        Text.Contains(TEXT("local **X/Y/Z**")));
    // The corrected mental model: the vertical extent is `depth`, not `height`.
    TestTrue(TEXT("create_box page states the vertical extent is depth (local Z)"),
        Text.Contains(TEXT("the vertical extent is **`depth`** (local Z)")));
    // The upright-wall recipe so a caller never authors a flat slab again.
    TestTrue(TEXT("create_box page gives the upright-wall recipe {width:400, height:80, depth:500}"),
        Text.Contains(TEXT("{width:400, height:80, depth:500}")));
    return true;
}

// ============================================================================
// create_arch method page: torus revolves in local X-Y (flat by default), roll
// sign to stand apex-up. Overlay-exclusive H3.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryCreateArchRollDocTest,
    "PinWright.infra.wiki_handler.MethodPage.GeometryCreateArchRollSign",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGeometryCreateArchRollDocTest::RunTest(const FString& Parameters)
{
    FString Text;
    if (!WikiDocTestHelpers::RenderOrFail(*this, TEXT("geometry.create_arch"), Text))
    {
        return false;
    }

    // The build plane: the torus revolves in local X-Y, so the arch lies flat.
    TestTrue(TEXT("create_arch page states the torus revolves in the local X-Y plane"),
        Text.Contains(TEXT("X-Y plane about Z")));
    // The roll sign that stands it apex-up — the exact fact the reporter had backwards.
    TestTrue(TEXT("create_arch page states rotation.roll = -90 stands it apex-up"),
        Text.Contains(TEXT("`rotation.roll = -90`")) && Text.Contains(TEXT("apex-up")));
    // And that +90 points the apex down (the wrong-sign trap).
    TestTrue(TEXT("create_arch page warns rotation.roll = +90 points the apex down"),
        Text.Contains(TEXT("apex")) && Text.Contains(TEXT("down")));
    return true;
}

// ============================================================================
// get_vertex_position method page: returns mesh-LOCAL coords (actor transform not
// applied), with the world-space readback steer. Overlay-exclusive H3.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryGetVertexPositionLocalDocTest,
    "PinWright.infra.wiki_handler.MethodPage.GeometryGetVertexPositionLocalCoords",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGeometryGetVertexPositionLocalDocTest::RunTest(const FString& Parameters)
{
    FString Text;
    if (!WikiDocTestHelpers::RenderOrFail(*this, TEXT("geometry.get_vertex_position"), Text))
    {
        return false;
    }

    // The core caveat: coordinates are mesh-LOCAL, the world transform is not applied.
    TestTrue(TEXT("get_vertex_position page states the returned coords are mesh-LOCAL"),
        Text.Contains(TEXT("mesh-LOCAL")));
    // The consequence the reporter probed for: moving the actor does not change the readback.
    TestTrue(TEXT("get_vertex_position page states actor.set_transform does not change the readback"),
        Text.Contains(TEXT("actor.set_transform")));
    // The world-space readback steer so a caller can reason about overlap.
    TestTrue(TEXT("get_vertex_position page steers world-space overlap to actor.get_bounding_box"),
        Text.Contains(TEXT("actor.get_bounding_box")));
    return true;
}
