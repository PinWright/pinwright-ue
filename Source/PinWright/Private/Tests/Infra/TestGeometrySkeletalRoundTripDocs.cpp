// Copyright (c) 2026 Alexander Penkin. MIT License.

// Doc-sync guard for the SkeletalMesh <-> DynamicMesh round trip.
//
// The registered param glosses already render "skeletonPath", "overwrite" and "maxInfluences", so
// Text.Contains on those alone would pass with the docs/wiki-src/geometry.md overlay reverted.
// These assertions pin only overlay-exclusive facts a caller cannot get from the param table, and
// each one is a fact whose absence costs real work:
//   - bind LAST, and WHICH ops are safe to run after a bind (the refinement-vs-append split);
//   - these verbs write BASE skinning while the skeleton.* weight verbs write a named profile
//     (an agent that does not know this reaches for skeleton.set_vertex_weights and silently
//     edits a channel the renderer ignores by default);
//   - creating onto an occupied path would have the engine empty the asset in place.
#include "Misc/AutomationTest.h"
#include "Tests/Infra/WikiDocTestHelpers.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometrySkeletalRoundTripNamespaceDocTest,
    "PinWright.infra.wiki_handler.NamespacePage.GeometrySkeletalRoundTrip",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGeometrySkeletalRoundTripNamespaceDocTest::RunTest(const FString& Parameters)
{
    FString Text;
    if (!WikiDocTestHelpers::RenderOrFail(*this, TEXT("geometry"), Text))
    {
        return false;
    }

    // The round trip is named as a set on the namespace page, not buried in three method pages.
    TestTrue(TEXT("geometry page names the skeletal load verb"),
        Text.Contains(TEXT("geometry.create_from_skeletal_mesh")));
    TestTrue(TEXT("geometry page names the bind verb"),
        Text.Contains(TEXT("geometry.bind_skin_weights")));
    TestTrue(TEXT("geometry page names the skeletal bake verb"),
        Text.Contains(TEXT("geometry.convert_to_skeletal_mesh")));

    // The ordering rule, and the actionable form of it. The page must not merely say "bind
    // last" - it has to tell the caller what to do when they edit anyway, because whether a
    // given op preserves weights is an implementation detail of that op.
    TestTrue(TEXT("geometry page states the bind must come last"),
        Text.Contains(TEXT("Bind last")) || Text.Contains(TEXT("bind LAST")));
    TestTrue(TEXT("geometry page tells the caller to re-bind after a geometry edit"),
        Text.Contains(TEXT("re-bind after every geometry edit")));
    TestTrue(TEXT("geometry page names the appending ops that do not carry weights"),
        Text.Contains(TEXT("append_triangle")));

    // Base skinning vs the skeleton.* named-profile channel. Overlay-only, and the single most
    // expensive thing on this page to learn the hard way.
    TestTrue(TEXT("geometry page states these verbs write base skinning"),
        Text.Contains(TEXT("base skinning")) || Text.Contains(TEXT("base skin weights")));
    TestTrue(TEXT("geometry page contrasts the skeleton.* named-profile verbs"),
        Text.Contains(TEXT("skin weight profile")) && Text.Contains(TEXT("skeleton.")));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryBindSkinWeightsDocTest,
    "PinWright.infra.wiki_handler.MethodPage.GeometryBindSkinWeights",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGeometryBindSkinWeightsDocTest::RunTest(const FString& Parameters)
{
    FString Text;
    if (!WikiDocTestHelpers::RenderOrFail(*this, TEXT("geometry.bind_skin_weights"), Text))
    {
        return false;
    }

    // The ordering rule and the observable that proves a re-bind happened.
    TestTrue(TEXT("page tells the caller to run the bind last"),
        Text.Contains(TEXT("Run this LAST")) || Text.Contains(TEXT("run this last")));
    TestTrue(TEXT("page names the rebound signal"), Text.Contains(TEXT("rebound")));

    // Coverage is measured, not echoed - so fullyWeighted:false is a real finding.
    TestTrue(TEXT("page states the reported coverage is a measured per-vertex scan"),
        Text.Contains(TEXT("verticesUnweighted")) && Text.Contains(TEXT("measured")));

    // The deliberate absence of a profile argument, which is the design decision most likely to
    // be "fixed" by someone who has not read why.
    TestTrue(TEXT("page states there is deliberately no profile argument"),
        Text.Contains(TEXT("no `profile` argument")));

    // The binding-method recommendation, stated as a mechanism rather than folklore.
    TestTrue(TEXT("page names both binding methods"),
        Text.Contains(TEXT("DirectDistance")) && Text.Contains(TEXT("GeodesicVoxel")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryConvertToSkeletalMeshDocTest,
    "PinWright.infra.wiki_handler.MethodPage.GeometryConvertToSkeletalMesh",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGeometryConvertToSkeletalMeshDocTest::RunTest(const FString& Parameters)
{
    FString Text;
    if (!WikiDocTestHelpers::RenderOrFail(*this, TEXT("geometry.convert_to_skeletal_mesh"), Text))
    {
        return false;
    }

    // The two rejection codes, and the fact that they exist because the ENGINE does not reject.
    TestTrue(TEXT("page names the unbound-mesh rejection"), Text.Contains(TEXT("NO_SKIN_WEIGHTS")));
    TestTrue(TEXT("page names the stale-binding rejection"),
        Text.Contains(TEXT("SKIN_WEIGHTS_INCOMPLETE")));
    TestTrue(TEXT("page states the engine itself does not catch a partly-unskinned write"),
        Text.Contains(TEXT("does not check")) || Text.Contains(TEXT("Neither engine entry point")));

    // The collision guard, with the reason - "the engine wipes it in place" is the whole
    // justification for ASSET_EXISTS being an error rather than an implicit overwrite.
    TestTrue(TEXT("page names the occupied-path rejection"), Text.Contains(TEXT("ASSET_EXISTS")));
    TestTrue(TEXT("page states an unguarded create would empty the existing asset in place"),
        Text.Contains(TEXT("in place")) && Text.Contains(TEXT("reference skeleton")));

    // Materials: every imported section must have an in-range, non-null slot after either branch.
    TestTrue(TEXT("page documents section/material-slot coverage"),
        Text.Contains(TEXT("Every imported section")) && Text.Contains(TEXT("materialSlots")));
    TestTrue(TEXT("page states the overwrite path preserves them"),
        Text.Contains(TEXT("materialsPreserved")));

    // Honest persistence: neither engine call writes the .uasset.
    TestTrue(TEXT("page states neither engine call writes the .uasset itself"),
        Text.Contains(TEXT("Neither engine call writes")));
    return true;
}
