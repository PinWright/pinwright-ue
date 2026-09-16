// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for E-cloth-verbs-wiki-overpromise-create-section:
// The two cloth verbs in SkeletalMeshHandler.cpp overpromised in their
// REGISTER_RPC_HANDLER summaries, with no skeleton.md overlay to caveat them:
//   - skeleton.bind_cloth_to_skeletal_mesh was summarized "Create and bind a
//     UClothingAsset..." but the handler never creates: with clothAssetName set it
//     looks up an EXISTING asset and returns CLOTH_NOT_FOUND when absent
//     (SkeletalMeshHandler.cpp ~L851-868), and with the name omitted it merely lists
//     existing clothing assets (~L889-910).
//   - skeleton.assign_cloth_asset_to_mesh was summarized "Attach an existing
//     UClothingAsset to a specific section ... or share a cloth asset between
//     sections," but at the time its ParamSpec declared ONLY skeletalMeshPath and the body
//     just listed, so clothAssetName/sectionIndex were rejected UNKNOWN_PARAMS.
//
// Update: F-cloth-create-and-section-assign (#3-capability-implemented) since SHIPPED the
// section-assign capability for assign_cloth_asset_to_mesh — it now binds a named existing
// asset to a section (clothAssetName + sectionIndex) and lists when the name is omitted, so
// its "Attach an existing UClothingAsset to a specific section" summary is now accurate and
// the old list-only assertions here were superseded (the assign half below asserts the real
// attach-or-list behavior). The bind verb remains bind/list-only.
//
// The fix corrects both C++ summaries (bind: real bind-existing / list-only; assign: real
// attach-existing-to-section / list) AND adds the two missing
// `### skeleton.bind_cloth_to_skeletal_mesh` / `### skeleton.assign_cloth_asset_to_mesh`
// H3 overlay sections to docs/wiki-src/skeleton.md (which previously documented only
// describe_mesh / list_bones / describe_skin_weights).
//
// Both assertions render through the live WikiHandler::RenderPage method-page path (the
// same entry the HTTP gateway serves doc requests from): the corrected summary text is
// emitted into the page body from the registered FHandlerRegistration.Summary, and the
// overlay note surfaces via WikiOverlay::LoadMethodSection — not a copy of either string.
// The H3 headings are deliberately PLAIN (no backticks): LoadMethodSection keys a section
// on the exact text after `### ` with no backtick stripping, so only an un-backticked
// `### skeleton.bind_cloth_to_skeletal_mesh` matches the registered method name (the same
// reason skeleton.list_bones uses a plain heading).
//
// Reverting the summary edit removes the "does not create" / "ONLY skeletalMeshPath"
// wording; reverting (or re-backticking) the overlay makes LoadMethodSection return empty
// and drops the "no MCP verb ... creates" note — either regression fails these assertions.
#include "Misc/AutomationTest.h"
#include "Tests/Infra/WikiDocTestHelpers.h"

// ============================================================================
// skeleton.bind_cloth_to_skeletal_mesh method page: the corrected summary must say it
// binds/lists an EXISTING asset (CLOTH_NOT_FOUND when absent) and the overlay must state
// that no MCP verb creates a UClothingAsset — neither claims to "Create and bind".
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FClothBindHonestSummaryDocTest,
    "PinWright.infra.wiki_handler.MethodPage.ClothBindHonestSummary",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FClothBindHonestSummaryDocTest::RunTest(const FString& Parameters)
{
    FString Text;
    if (!WikiDocTestHelpers::RenderOrFail(*this, TEXT("skeleton.bind_cloth_to_skeletal_mesh"), Text))
    {
        return false;
    }

    // The corrected C++ summary (rendered from the registration) must NOT promise creation
    // and MUST describe the real bind-existing-or-list behavior plus CLOTH_NOT_FOUND.
    TestFalse(TEXT("bind page no longer claims it creates a cloth asset (\"Create and bind\")"),
        Text.Contains(TEXT("Create and bind")));
    TestTrue(TEXT("bind page summary describes binding an already-existing asset, not creating one"),
        Text.Contains(TEXT("ALREADY-EXISTING")) && Text.Contains(TEXT("CLOTH_NOT_FOUND")));

    // The overlay H3 (surfaced only via LoadMethodSection on the plain heading) must state
    // the cross-cutting truth: no MCP verb creates a UClothingAsset. Board-ticket IDs were
    // stripped from wiki-src (it ships to Fab users), so the overlay-exclusive marker is
    // the no-create-cloth-verb caveat, not a ticket cross-reference.
    TestTrue(TEXT("bind page overlay states no MCP verb creates a UClothingAsset"),
        Text.Contains(TEXT("no MCP verb")) && Text.Contains(TEXT("create")));
    TestTrue(TEXT("bind page overlay states there is no create-cloth verb today"),
        Text.Contains(TEXT("no create-cloth verb")));
    return true;
}

// ============================================================================
// skeleton.assign_cloth_asset_to_mesh method page: the section-assign CAPABILITY since
// shipped under F-cloth-create-and-section-assign (#3-capability-implemented) — the verb
// now binds a named existing UClothingAsset to a section (and lists when the name is
// omitted), so its summary correctly says it attaches to a specific section. The earlier
// "list-only / accepts ONLY skeletalMeshPath / clothAssetName+sectionIndex rejected
// UNKNOWN_PARAMS" wording the E-cloth docs ticket added is now FALSE for this verb and was
// superseded; only the bind verb above remains bind/list-only. This test asserts the page
// now describes the real attach-or-list behavior, the asset must already exist
// (CLOTH_NOT_FOUND, no create), and still cross-references the cloth capability ticket.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FClothAssignHonestSummaryDocTest,
    "PinWright.infra.wiki_handler.MethodPage.ClothAssignHonestSummary",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FClothAssignHonestSummaryDocTest::RunTest(const FString& Parameters)
{
    FString Text;
    if (!WikiDocTestHelpers::RenderOrFail(*this, TEXT("skeleton.assign_cloth_asset_to_mesh"), Text))
    {
        return false;
    }

    // The C++ summary (rendered from the registration) must describe the real
    // section-assign behavior shipped under F-cloth-create-and-section-assign: it attaches
    // an existing asset to a specific section, accepting clothAssetName + sectionIndex.
    TestTrue(TEXT("assign page summary states it attaches an existing asset to a specific section"),
        Text.Contains(TEXT("Attach an existing")) && Text.Contains(TEXT("specific section")));
    TestTrue(TEXT("assign page documents the clothAssetName + sectionIndex attach params"),
        Text.Contains(TEXT("clothAssetName")) && Text.Contains(TEXT("sectionIndex")));

    // The overlay H3 must caveat that the asset must already exist (CLOTH_NOT_FOUND, no
    // create) — overlay-only text. Board-ticket IDs were stripped from wiki-src (it ships
    // to Fab users), so the overlay-exclusive marker is the no-MCP-verb-creates caveat.
    TestTrue(TEXT("assign page overlay notes the asset must already exist (CLOTH_NOT_FOUND, no create)"),
        Text.Contains(TEXT("CLOTH_NOT_FOUND")) && Text.Contains(TEXT("does **not**")));
    TestTrue(TEXT("assign page overlay states no MCP verb creates a UClothingAsset"),
        Text.Contains(TEXT("no MCP verb does")));
    return true;
}
