// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for E-asset-save-missing-from-overlays:
// asset.save {assetPath, force?} is the narrow single-package writer (registered
// AssetSaveHandler.cpp:34), but the canonical save-recipe overlay
// (docs/wiki-src/safe-mutation-save.md) omitted it: recipe step 5 and the
// "Choosing The Writer" table named only editor.save_all / level.save /
// level.save_as, with no single-asset row — even though step 2 preaches "pick the
// narrowest writer." An agent reading the recipe therefore fell back to the blunt
// editor.save_all, which flushes EVERY dirty package instead of just the one it
// mutated. The fix adds asset.save to step 5 and the table as the targeted
// single-package alternative, and adds a `### asset.save` H3 to docs/wiki-src/asset.md
// so the call("asset.save") method page carries a Notes section (recipe cross-link
// + the saved/pendingFlush/integrityGate readback contract).
//
// This renders the LIVE pages through WikiHandler::RenderPage (the same entry the
// HTTP gateway uses for doc requests), not a copy of the overlay text, so it
// exercises the real overlay loader + renderer. The safe-mutation-save topic page
// body is the overlay prelude verbatim (RenderTopicPage -> LoadGroupPrelude; the
// file has no `### ` H3, so its whole body is the prelude), meaning every marker
// asserted on it is overlay-only content — reverting the overlay edit removes the
// asset.save row/step and these assertions fail. The method-page assertion asserts
// only on the recipe cross-link "safe-mutation-save", which the auto-generated
// macro summary (AssetSaveHandler.cpp:35-41) does NOT contain, so it can pass only
// when the `### asset.save` overlay H3 renders under `## Notes`
// (WikiHandler.cpp RenderMethodPage -> LoadMethodSection).
#include "Misc/AutomationTest.h"
#include "Tests/Infra/WikiDocTestHelpers.h"

// ============================================================================
// The safe-mutation-save recipe surfaces asset.save as the narrow single-package
// writer, and the asset.save method page carries the enriching overlay Notes.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSafeMutationSaveAssetSaveDocTest,
    "PinWright.infra.wiki_handler.TopicPage.SafeMutationSaveAssetSave",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSafeMutationSaveAssetSaveDocTest::RunTest(const FString& Parameters)
{
    // --- The canonical save-recipe page now names the single-package writer. ---
    FString RecipeText;
    if (!WikiDocTestHelpers::RenderOrFail(*this, TEXT("safe-mutation-save"), RecipeText))
    {
        return false;
    }

    // Core: the recipe names asset.save. The whole topic-page body is overlay
    // content, so this substring proves the recipe now documents the writer;
    // reverting both the step-5 bullet and the table row removes it and this fails.
    TestTrue(TEXT("safe-mutation-save recipe names the asset.save single-package writer"),
        RecipeText.Contains(TEXT("asset.save")));

    // The ergonomic point: asset.save is framed as the narrow alternative to the
    // blanket flush. "every dirty package" is distinctive to the added table row
    // (the original editor.save_all row said "dirty worlds and content packages",
    // never "every dirty package"), so it pins the contrast-with-editor.save_all
    // framing rather than an incidental mention.
    TestTrue(TEXT("safe-mutation-save frames asset.save as the alternative to flushing every dirty package"),
        RecipeText.Contains(TEXT("every dirty package")));

    // --- The asset.save method page carries the enriching overlay Notes. ---
    FString MethodText;
    if (!WikiDocTestHelpers::RenderOrFail(*this, TEXT("asset.save"), MethodText))
    {
        return false;
    }

    // The `### asset.save` overlay H3 cross-links the recipe. "safe-mutation-save"
    // is overlay-only: the auto-generated macro summary the method page renders
    // above the Notes section does not reference the recipe, so this passes only
    // when LoadMethodSection surfaces the asset.md H3.
    TestTrue(TEXT("asset.save method page cross-links the safe-mutation-save recipe via its overlay Notes"),
        MethodText.Contains(TEXT("safe-mutation-save")));

    return true;
}
