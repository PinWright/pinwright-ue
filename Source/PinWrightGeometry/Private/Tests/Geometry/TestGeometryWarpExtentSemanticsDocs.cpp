// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for E-geometry-warp-extent-semantics:
// geometry.twist / geometry.taper / geometry.bend each take an `extent` whose
// auto-generated summary is a bare "Twist/Flare/Bend extent (default 50)", naming
// neither the units (world units, not a 0..1 fraction) nor the symmetry. Symmetry is
// a REQUEST PARAMETER, not a constant: all three read `symmetricExtents` and
// `lowerExtent` through ReadWarpExtentSpec (MeshOpsHandler.cpp:180-181, called at
// :1214 bend / :1259 twist / :1313 taper) and publish both parameters
// (:1198-1199 / :1243-1244 / :1289-1290). `symmetricExtents` defaults true
// (GeometryOps_Modeling.h:651), so by default `extent` is a half-extent symmetric
// about the origin spanning [-extent, +extent], and a height-H centered mesh is
// covered at extent ≈ H/2. `symmetricExtents: false` spans [-lowerExtent, +extent]
// instead, with `lowerExtent` (default 10, GeometryOps_Modeling.h:654) read only then.
//
// An earlier revision of this file asserted the C++ field name `bSymmetricExtents=true`
// on each page. That described the code once and is wrong twice over now
// (GeometryOps_Modeling.h:645, "was hardcoded true"): the field is not what a caller
// passes, and true is no longer the only value it takes. So the assertions below are on
// the SUBSTANCE - the parameter name and the two SPANS - and not on any sentence.
// Reworded prose keeps passing; a page that stops saying what a setting spans does not.
//
// The doc surface is a `## Warp deformers (twist / taper / bend)` namespace section plus
// `### geometry.twist` / `### geometry.taper` / `### geometry.bend` H3 overlay sections
// in docs/wiki-src/geometry.md, surfaced by WikiOverlay (namespace `##` section ->
// namespace page; `### method` H3 -> per-method page). These tests exercise the live
// WikiHandler::RenderPage render path - the same entry the HTTP gateway uses for doc
// requests - not a copy of the overlay text.
//
// Which markers are overlay-exclusive, since that is what makes a revert fail:
// "world units", "extent ≈ H/2" and taper's "not a percentage" appear in no
// auto-generated text, so deleting the overlay sections makes LoadGroupPrelude /
// LoadMethodSection return empty and those assertions fail. The two span notations are
// deliberately NOT overlay-exclusive - MeshOpsHandler's published `symmetricExtents`
// description carries them too, and a method page that states the semantics from either
// surface is a correct page.
#include "Misc/AutomationTest.h"
#include "Tests/Infra/WikiDocTestHelpers.h"

// ============================================================================
// Namespace page: the `## Warp deformers` section discloses the shared convention
// once (world units + symmetric half-extent + cover-H ⇒ extent ≈ H/2). These
// markers live only in the namespace `##` section of docs/wiki-src/geometry.md.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryWarpExtentNamespaceDocTest,
    "PinWright.infra.wiki_handler.Namespace.GeometryWarpExtentSemantics",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGeometryWarpExtentNamespaceDocTest::RunTest(const FString& Parameters)
{
    FString Text;
    if (!WikiDocTestHelpers::RenderOrFail(*this, TEXT("geometry"), Text))
    {
        return false;
    }

    // The warp-deformer section must exist on the namespace page.
    TestTrue(TEXT("geometry page carries a Warp deformers section"),
        Text.Contains(TEXT("Warp deformers")));
    // Units: extent is in world units, not a 0..1 fraction.
    TestTrue(TEXT("geometry page states extent is in world units"),
        Text.Contains(TEXT("world units")));
    // Symmetry: `symmetricExtents` defaults true, spanning [-extent, +extent];
    // false spans [-lowerExtent, +extent].
    TestTrue(TEXT("geometry page states extent is a symmetric half-extent"),
        Text.Contains(TEXT("symmetric half-extent")));
    // The actionable rule: cover a height-H centered mesh with extent ≈ H/2.
    TestTrue(TEXT("geometry page gives the cover-H ⇒ extent ≈ H/2 rule"),
        Text.Contains(TEXT("extent ≈ H/2")));
    // And the other setting, which the shared section is the only place to state once:
    // symmetry is a knob, so the page has to say what turning it off spans.
    TestTrue(TEXT("geometry page gives the symmetricExtents:false span"),
        Text.Contains(TEXT("[-lowerExtent, +extent]")));
    return true;
}

// ============================================================================
// twist method page: world-unit symmetric half-extent, cover-H ⇒ extent ≈ H/2,
// with the 300-tall column example. Overlay-exclusive H3.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryTwistExtentDocTest,
    "PinWright.infra.wiki_handler.MethodPage.GeometryTwistExtentSemantics",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGeometryTwistExtentDocTest::RunTest(const FString& Parameters)
{
    FString Text;
    if (!WikiDocTestHelpers::RenderOrFail(*this, TEXT("geometry.twist"), Text))
    {
        return false;
    }

    TestTrue(TEXT("twist page states extent is in world units"),
        Text.Contains(TEXT("world units")));
    // Substance, not the engine field name: the knob a caller sets, and what each
    // of its two settings SPANS. `symmetricExtents` is the request parameter;
    // `bSymmetricExtents` is the C++ field it feeds, which no caller ever writes.
    TestTrue(TEXT("twist page names the symmetricExtents parameter"),
        Text.Contains(TEXT("symmetricExtents")));
    TestTrue(TEXT("twist page gives the default span [-extent, +extent]"),
        Text.Contains(TEXT("[-extent, +extent]")));
    TestTrue(TEXT("twist page gives the symmetricExtents:false span [-lowerExtent, +extent]"),
        Text.Contains(TEXT("[-lowerExtent, +extent]")));
    // The symmetric half-extent phrasing (overlay-exclusive, not just the flag name).
    TestTrue(TEXT("twist page states extent is a symmetric half-extent about the origin"),
        Text.Contains(TEXT("symmetric half-extent")));
    TestTrue(TEXT("twist page gives the cover-H ⇒ extent ≈ H/2 rule"),
        Text.Contains(TEXT("extent ≈ H/2")));
    return true;
}

// ============================================================================
// taper method page: same world-unit symmetric half-extent convention, plus the
// flareX/flareY-vs-extent distinction. Overlay-exclusive H3.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryTaperExtentDocTest,
    "PinWright.infra.wiki_handler.MethodPage.GeometryTaperExtentSemantics",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGeometryTaperExtentDocTest::RunTest(const FString& Parameters)
{
    FString Text;
    if (!WikiDocTestHelpers::RenderOrFail(*this, TEXT("geometry.taper"), Text))
    {
        return false;
    }

    TestTrue(TEXT("taper page states extent is in world units"),
        Text.Contains(TEXT("world units")));
    // Substance, not the engine field name: the knob a caller sets, and what each
    // of its two settings SPANS. `symmetricExtents` is the request parameter;
    // `bSymmetricExtents` is the C++ field it feeds, which no caller ever writes.
    TestTrue(TEXT("taper page names the symmetricExtents parameter"),
        Text.Contains(TEXT("symmetricExtents")));
    TestTrue(TEXT("taper page gives the default span [-extent, +extent]"),
        Text.Contains(TEXT("[-extent, +extent]")));
    TestTrue(TEXT("taper page gives the symmetricExtents:false span [-lowerExtent, +extent]"),
        Text.Contains(TEXT("[-lowerExtent, +extent]")));
    TestTrue(TEXT("taper page states extent is a symmetric half-extent about the origin"),
        Text.Contains(TEXT("symmetric half-extent")));
    TestTrue(TEXT("taper page gives the cover-H ⇒ extent ≈ H/2 rule"),
        Text.Contains(TEXT("extent ≈ H/2")));
    // The flareX/flareY-vs-extent trap: flare params are percentages, extent is a world length.
    TestTrue(TEXT("taper page distinguishes flareX/flareY percentages from the world-unit extent"),
        Text.Contains(TEXT("flareX")) && Text.Contains(TEXT("not a percentage")));
    return true;
}

// ============================================================================
// bend method page: same world-unit symmetric half-extent convention, plus the
// angle-vs-extent distinction. Overlay-exclusive H3.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryBendExtentDocTest,
    "PinWright.infra.wiki_handler.MethodPage.GeometryBendExtentSemantics",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGeometryBendExtentDocTest::RunTest(const FString& Parameters)
{
    FString Text;
    if (!WikiDocTestHelpers::RenderOrFail(*this, TEXT("geometry.bend"), Text))
    {
        return false;
    }

    TestTrue(TEXT("bend page states extent is in world units"),
        Text.Contains(TEXT("world units")));
    // Substance, not the engine field name: the knob a caller sets, and what each
    // of its two settings SPANS. `symmetricExtents` is the request parameter;
    // `bSymmetricExtents` is the C++ field it feeds, which no caller ever writes.
    TestTrue(TEXT("bend page names the symmetricExtents parameter"),
        Text.Contains(TEXT("symmetricExtents")));
    TestTrue(TEXT("bend page gives the default span [-extent, +extent]"),
        Text.Contains(TEXT("[-extent, +extent]")));
    TestTrue(TEXT("bend page gives the symmetricExtents:false span [-lowerExtent, +extent]"),
        Text.Contains(TEXT("[-lowerExtent, +extent]")));
    TestTrue(TEXT("bend page states extent is a symmetric half-extent about the origin"),
        Text.Contains(TEXT("symmetric half-extent")));
    TestTrue(TEXT("bend page gives the cover-H ⇒ extent ≈ H/2 rule"),
        Text.Contains(TEXT("extent ≈ H/2")));
    return true;
}
