// Copyright (c) 2026 Alexander Penkin. MIT License.

// Doc-sync guard for the base-skinning-vs-alternate-profile honesty, following the
// TestClothVerbHonestSummaryDocs.cpp precedent for the same class of defect: a verb whose
// summary oversold what it did.
//
// THE DEFECT THESE PIN
//
// All four skeleton weight mutators (normalize / prune / set_vertex / copy) write a NAMED
// ALTERNATE skin-weight profile — the engine's "alternate influences" channel, which nothing
// uses unless a component activates it by name — and never touch FSkelMeshSection::SoftVertices,
// the base skinning the renderer uses. Their summaries said "skin weights", which a caller
// reasonably reads as base skinning. Worse, skeleton.describe_skin_weights was documented as
// their verification counterpart and read back the same profile, so the write and the check
// agreed with each other while both disagreed with the mesh.
//
// A summary reverted to "skin weights", or an overlay reverted to describing only profiles,
// re-opens exactly that loop with no failing behavioural test — the behaviour would be
// correct and the description wrong, which is what made this survivable in the first place.
// These assertions are the only thing that fails on such a revert.
//
// Everything below renders through the live WikiHandler::RenderPage path (the same entry the
// gateway serves doc requests from): summary text comes from the registered
// FHandlerRegistration.Summary, overlay text from WikiOverlay::LoadMethodSection. Neither is a
// copy of a string held in this file.
//
// Overlay H3 headings must stay PLAIN (no backticks): LoadMethodSection keys a section on the
// exact text after `### ` with no backtick stripping, so `### skeleton.normalize_weights`
// matches and a backticked variant silently matches nothing.

#include "Misc/AutomationTest.h"
#include "Tests/Infra/WikiDocTestHelpers.h"

namespace
{
    // Every mutator page must (a) name the alternate profile as its write target and (b) say
    // base skinning is not modified. Shared so a fifth mutator cannot be added with a page that
    // silently omits the disclosure. SkinHonesty-prefixed per the Unity ODR house rule.
    void SkinHonestyAssertMutatorPage(FAutomationTestBase& Test, const TCHAR* Method)
    {
        FString Text;
        if (!WikiDocTestHelpers::RenderOrFail(Test, Method, Text))
        {
            return;
        }

        Test.TestTrue(*FString::Printf(TEXT("%s page names its write target as an ALTERNATE profile"), Method),
            Text.Contains(TEXT("ALTERNATE")));
        Test.TestTrue(*FString::Printf(TEXT("%s page states base skinning is not modified"), Method),
            Text.Contains(TEXT("base skinning")) || Text.Contains(TEXT("Base skinning")));
        // The machine-readable half of the same claim, so an agent that reads only the response
        // shape is told as plainly as one that reads prose.
        Test.TestTrue(*FString::Printf(TEXT("%s page documents the baseSkinningModified disclosure field"), Method),
            Text.Contains(TEXT("baseSkinningModified")));
    }
}

// ============================================================================
// The four mutators must each disclose that they write an alternate profile.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSkinWeightMutatorHonestyDocTest,
    "PinWright.infra.wiki_handler.MethodPage.SkinWeightMutatorsDiscloseProfileWrite",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSkinWeightMutatorHonestyDocTest::RunTest(const FString& Parameters)
{
    SkinHonestyAssertMutatorPage(*this, TEXT("skeleton.normalize_weights"));
    SkinHonestyAssertMutatorPage(*this, TEXT("skeleton.prune_weights"));
    SkinHonestyAssertMutatorPage(*this, TEXT("skeleton.set_vertex_weights"));
    SkinHonestyAssertMutatorPage(*this, TEXT("skeleton.copy_weights"));
    return true;
}

// ============================================================================
// skeleton.describe_skin_weights must document the BASE block, not just profiles — it is the
// verb the mutator docs point at for verification, and reporting profiles alone is what closed
// the loop.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSkinWeightReadbackDocumentsBaseDocTest,
    "PinWright.infra.wiki_handler.MethodPage.DescribeSkinWeightsDocumentsBaseSkinning",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSkinWeightReadbackDocumentsBaseDocTest::RunTest(const FString& Parameters)
{
    FString Text;
    if (!WikiDocTestHelpers::RenderOrFail(*this, TEXT("skeleton.describe_skin_weights"), Text))
    {
        return false;
    }

    TestTrue(TEXT("readback page documents the baseSkinning block"),
        Text.Contains(TEXT("baseSkinning")));
    TestTrue(TEXT("readback page names the section soft-vertices as the base source"),
        Text.Contains(TEXT("sectionSoftVertices")) || Text.Contains(TEXT("section soft-vertices")));
    // Without this the caller is handed bone numbers in an unstated space, which is the
    // ambiguity that let section-local slots be read as reference-skeleton indices for so long.
    TestTrue(TEXT("readback page documents boneIndexSpace on every block"),
        Text.Contains(TEXT("boneIndexSpace")));
    TestTrue(TEXT("readback page states profiles[] are ALTERNATE influence sets"),
        Text.Contains(TEXT("alternate")) || Text.Contains(TEXT("ALTERNATE")));
    // "I could not look" must not render as "I looked and there was nothing".
    TestTrue(TEXT("readback page documents the NO_LOD_MODELS refusal"),
        Text.Contains(TEXT("NO_LOD_MODELS")));
    return true;
}

// ============================================================================
// The namespace page must carry the base-vs-profile explanation and the base-weight write
// recipe ABOVE its first `###` — rendering stops at the first `###`, so a section written
// below it is invisible on the page it was written for.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSkinWeightNamespaceGuidanceDocTest,
    "PinWright.infra.wiki_handler.NamespacePage.SkeletonExplainsBaseVsProfileSkinning",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSkinWeightNamespaceGuidanceDocTest::RunTest(const FString& Parameters)
{
    FString Text;
    if (!WikiDocTestHelpers::RenderOrFail(*this, TEXT("skeleton"), Text))
    {
        return false;
    }

    // ExtractSection anchors on "## <Heading>\n". The overlay source is authored with LF but
    // git's autocrlf can hand the loader CRLF, which would put a \r between the heading and the
    // newline and make the anchor miss — a silently skipped assertion rather than a failure.
    // Normalizing here costs nothing and removes that whole class of false green.
    Text.ReplaceInline(TEXT("\r"), TEXT(""));

    FString Section;
    if (!TestTrue(TEXT("skeleton page renders the skin-weights section (it must sit above the first ###)"),
            WikiDocTestHelpers::ExtractSection(Text,
                TEXT("Skin weights: base skinning vs. alternate profiles"), Section)))
    {
        return false;
    }

    TestTrue(TEXT("the section distinguishes base skinning from named alternate profiles"),
        Section.Contains(TEXT("SoftVertices")) && Section.Contains(TEXT("alternate")));
    TestTrue(TEXT("the section states all four mutators leave base skinning untouched"),
        Section.Contains(TEXT("normalize_weights")) && Section.Contains(TEXT("copy_weights"))
        && Section.Contains(TEXT("untouched")));
    // The whole point of the section: telling a caller what to do INSTEAD.
    TestTrue(TEXT("the section names the geometry round trip as the base-weight write path"),
        Section.Contains(TEXT("geometry.create_from_skeletal_mesh"))
        && Section.Contains(TEXT("geometry.bind_skin_weights"))
        && Section.Contains(TEXT("geometry.convert_to_skeletal_mesh")));
    TestTrue(TEXT("the section names how to verify a base-weight write"),
        Section.Contains(TEXT("audit_skin_weights")) || Section.Contains(TEXT("baseSkinning")));
    // Both bone index spaces, named. A bone number without its space is the defect this
    // namespace kept reproducing.
    TestTrue(TEXT("the section defines both bone index spaces"),
        Section.Contains(TEXT("Reference-skeleton")) && Section.Contains(TEXT("Section-local")));
    return true;
}
