// Copyright (c) 2026 Alexander Penkin. MIT License.

// Doc coverage for the two deformer parameters that used to be hardcoded.
//
// Sibling of TestGeometryWarpExtentSemanticsDocs.cpp, which pins the extent's UNITS and SYMMETRY.
// This file pins the other half of the same story - WHERE the extent is measured from and along
// WHICH axis - plus noise_deform's magnitude reading, and it exists for the same reason: a caller
// who cannot tell from the page that `axis` exists will keep rotating meshes into Z and back, and
// a caller who cannot tell that `relative` scales by a LOCAL quantity will read `magnitude: 0.5`
// as half a unit.
//
// Assertions are on SUBSTANCE, not on sentences, following that file's precedent: the parameter
// names, the two things `center` is not (a bounding-box centre), the proxy's stated failure mode,
// and the two vertex shapes whose behaviour is not guessable. Reworded prose keeps passing; a page
// that stops saying what a parameter measures does not.
#include "Misc/AutomationTest.h"
#include "Tests/Infra/WikiDocTestHelpers.h"

// ============================================================================
// Namespace page: `axis` and `center` are disclosed once, in the shared section
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryWarpFrameNamespaceDocTest,
    "PinWright.infra.wiki_handler.Namespace.GeometryWarpFrameSemantics",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGeometryWarpFrameNamespaceDocTest::RunTest(const FString& Parameters)
{
    FString Text;
    if (!WikiDocTestHelpers::RenderOrFail(*this, TEXT("geometry"), Text))
    {
        return false;
    }

    TestTrue(TEXT("the geometry page names the warp axis parameter"),
        Text.Contains(TEXT("`axis`")));
    TestTrue(TEXT("the geometry page names the warp centre parameter"),
        Text.Contains(TEXT("`center`")));

    // The defect the two parameters fix. Without this a reader cannot tell that the hardcoded
    // frame was a limitation rather than a convention.
    TestTrue(TEXT("the geometry page says the extent is measured along the chosen axis"),
        Text.Contains(TEXT("extent spans")) || Text.Contains(TEXT("extent along")));

    // `center` is the axis LINE. Publishing it as a bounding-box centre would be a different
    // parameter that silently moves when an earlier op changes the mesh extent.
    TestTrue(TEXT("the geometry page says the centre is not a bounding-box centre"),
        Text.Contains(TEXT("bounding-box centre")));

    // The compatibility promise: an existing request is unchanged.
    TestTrue(TEXT("the geometry page states the defaults preserve today's behaviour"),
        Text.Contains(TEXT("identity frame")));

    return true;
}

// ============================================================================
// noise_deform method page: the two magnitude readings, and relative's three catches
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryNoiseMagnitudeModeDocTest,
    "PinWright.infra.wiki_handler.MethodPage.GeometryNoiseMagnitudeMode",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGeometryNoiseMagnitudeModeDocTest::RunTest(const FString& Parameters)
{
    FString Text;
    if (!WikiDocTestHelpers::RenderOrFail(*this, TEXT("geometry.noise_deform"), Text))
    {
        return false;
    }

    TestTrue(TEXT("the page names both magnitude readings"),
        Text.Contains(TEXT("absolute")) && Text.Contains(TEXT("relative")));

    // The quantity itself. "A fraction" alone would leave a fraction OF WHAT to be guessed.
    TestTrue(TEXT("the page says what relative is a fraction of"),
        Text.Contains(TEXT("mean one-ring edge length")));

    // Absolute is the engine call, so existing results are reproduced rather than approximated.
    TestTrue(TEXT("the page states absolute reproduces existing results"),
        Text.Contains(TEXT("engine call unchanged")));

    // The proxy's stated failure mode - the one case where relative silently becomes absolute.
    TestTrue(TEXT("the page states where the feature-size proxy stops holding"),
        Text.Contains(TEXT("uniformly remeshed")));

    // The two vertex shapes whose behaviour cannot be guessed from "mean one-ring edge length".
    TestTrue(TEXT("the page says what a boundary vertex does"),
        Text.Contains(TEXT("boundary vertex")));
    TestTrue(TEXT("the page says a split vertex can open a seam"),
        Text.Contains(TEXT("opens")));

    // And the refusal, so a caller meets it in the docs rather than in an error.
    TestTrue(TEXT("the page states the applyAlongNormal requirement"),
        Text.Contains(TEXT("applyAlongNormal")));

    return true;
}
