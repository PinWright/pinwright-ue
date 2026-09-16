// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for board ticket B-trace-complex-hits-render-geometry.
//
// The hazard is engine-correct behaviour: bTraceComplex resolves against COMPLEX
// collision, and for a StaticMesh with no simple collision primitives the complex
// representation is the render triangle soup - so a collisionless foliage mesh blocks a
// downward ground probe and the caller gets a plausible, wrong height. Nothing in the
// engine can be fixed here; what was defective was the API, which offered no way to say
// "I only care about the terrain" and no signal that this had happened.
//
// The code half of the fix lives in RaycastHandler.cpp / SpatialTraceUtils (multiHit +
// onlyActors/actorFilter/onlyClasses + the simpleCollisionShapes / renderGeometryHit /
// warnings diagnostics) and is covered by Tests/Spatial/TestRaycastHandlers.cpp. This
// test covers the DOC half, which is the part a caller hits first: the warning must be
// readable both from the namespace page (`call("spatial")`) and from the method page
// (`call("spatial.raycast")`), rendered through WikiHandler::RenderPage - the same entry
// the gateway serves doc requests from - not from a copy of the overlay text.
//
// Marker choice: the handler's own summary and param descriptions already render into the
// method page, and they mention traceComplex, render triangles, renderGeometryHit,
// simpleCollisionShapes, filteredOut and LandscapeProxy. Asserting on those words would
// pass with the overlay deleted, so every marker below is overlay-exclusive.
#include "Misc/AutomationTest.h"
#include "Tests/Infra/WikiDocTestHelpers.h"

// ============================================================================
// Namespace page: the `## traceComplex hits render geometry` section. This is the
// discovery surface an agent reads before it writes a height probe, and H3 method
// sections do NOT render here - so the warning has to exist as its own `##` block.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSpatialTraceComplexWarningDocTest,
    "PinWright.infra.wiki_handler.NamespacePage.SpatialTraceComplexWarning",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSpatialTraceComplexWarningDocTest::RunTest(const FString& Parameters)
{
    FString Text;
    if (!WikiDocTestHelpers::RenderOrFail(*this, TEXT("spatial"), Text))
    {
        return false;
    }

    // The rule itself, stated as a rule and not as a bug report.
    TestTrue(TEXT("spatial page states a collisionless mesh still blocks a complex trace"),
        Text.Contains(TEXT("traceComplex")) && Text.Contains(TEXT("render triangle soup")));

    // Framing matters: an agent that reads this as an engine bug will go looking for a
    // workaround instead of using the filters.
    TestTrue(TEXT("spatial page frames the behaviour as expected, not as a defect"),
        Text.Contains(TEXT("expected Unreal behaviour")));

    // The measured evidence, which is what makes the warning land.
    TestTrue(TEXT("spatial page carries the observed hit rate"),
        Text.Contains(TEXT("268 of 930")));

    // And the actual remedies, by parameter name.
    TestTrue(TEXT("spatial page names the filter parameters as the remedy"),
        Text.Contains(TEXT("onlyActors")) && Text.Contains(TEXT("actorFilter"))
            && Text.Contains(TEXT("onlyClasses")));
    TestTrue(TEXT("spatial page offers multiHit as the inspect-the-layers alternative"),
        Text.Contains(TEXT("multiHit")));
    TestTrue(TEXT("spatial page names the response-side detection fields"),
        Text.Contains(TEXT("simpleCollisionShapes")) && Text.Contains(TEXT("renderGeometryHit")));

    return true;
}

// ============================================================================
// Method page: `### spatial.raycast` documents the new arguments and the full
// response shape. Markers are fields/phrases the auto-generated param docs never
// emit, so deleting the overlay fails this.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSpatialRaycastFilteringDocTest,
    "PinWright.infra.wiki_handler.MethodPage.SpatialRaycastFiltering",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSpatialRaycastFilteringDocTest::RunTest(const FString& Parameters)
{
    FString Text;
    if (!WikiDocTestHelpers::RenderOrFail(*this, TEXT("spatial.raycast"), Text))
    {
        return false;
    }

    // Response fields the param descriptions never name.
    TestTrue(TEXT("raycast page documents the faceIndex response field"),
        Text.Contains(TEXT("faceIndex")));
    TestTrue(TEXT("raycast page documents the filteredOutCount response field"),
        Text.Contains(TEXT("filteredOutCount")));
    TestTrue(TEXT("raycast page documents the enriched actor identity block"),
        Text.Contains(TEXT("internalName")));

    // The peel semantics a caller can be surprised by: one hit per ACTOR, so a ray
    // through a single mesh is one entry, not an entry/exit pair.
    TestTrue(TEXT("raycast page states the one-hit-per-actor consequence of peeling"),
        Text.Contains(TEXT("enters and exits one mesh")));

    // The non-obvious departure from the house list convention.
    TestTrue(TEXT("raycast page warns that maxHits 0 is not 'all' like actor.list's limit"),
        Text.Contains(TEXT("actor.list")) && Text.Contains(TEXT("maxHits")));

    // actorFilter shares actor.list's matchMode/caseSensitive policy rather than inventing
    // a per-verb rule; the page must say so, including the modifiers-without-a-pattern
    // rejection (the param docs mention INVALID_PATTERN but never INVALID_ARGUMENT).
    TestTrue(TEXT("raycast page documents the shared matchMode/caseSensitive knobs"),
        Text.Contains(TEXT("matchMode")) && Text.Contains(TEXT("caseSensitive")));
    TestTrue(TEXT("raycast page states modifiers without a pattern are rejected"),
        Text.Contains(TEXT("INVALID_ARGUMENT")));

    // And it must route the reader to the full hazard explanation on the namespace page.
    TestTrue(TEXT("raycast page cross-links the traceComplex hazard section"),
        Text.Contains(TEXT("traceComplex hits render geometry")));

    return true;
}
