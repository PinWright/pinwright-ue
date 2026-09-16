// Copyright (c) 2026 Alexander Penkin. MIT License.

// chooser.set_cell method page documents the per-column-kind value shapes: the
// single `value` param is dispatched four ways by the stored column kind
// (float={min,max}, bool literal/token, object/enum={value,comparison}). Rendering
// chooser.set_cell loads the `### chooser.set_cell` H3 overlay section from
// docs/wiki-src/chooser.md via WikiOverlay::LoadMethodSection.
// Counterfactual: if that H3 section in docs/wiki-src/chooser.md is reverted/removed,
// LoadMethodSection returns empty, the rendered page carries only the auto summary
// ("Set a chooser cell value using the stored column kind.") plus the param list, and
// the overlay-exclusive shape markers asserted below disappear. The markers are chosen
// to live ONLY in the overlay table, not in the auto summary or the param description:
// the param desc uses "{min,max}" (no space) and "object/enum={value,comparison}", so
// asserting on "{min, max}" (with a space) and the "assetPath" alias note pins the
// overlay specifically. This pins E-chooser-set-cell-value-shape-undocumented.
//
// Lives in PinWrightChooser (not the main module's TestWikiHandler.cpp) because the
// chooser.set_cell method page only exists when this integration module is loaded —
// on hosts without the Chooser plugin the page renders Not-Found by design.
#include "Misc/AutomationTest.h"
#include "Catalog/WikiHandler.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWikiHandlerSetCellDocumentsValueShapesTest,
    "PinWright.infra.wiki_handler.MethodPage.SetCellDocumentsValueShapes",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWikiHandlerSetCellDocumentsValueShapesTest::RunTest(const FString& Parameters)
{
    FString Text;
    if (!WikiHandler::RenderPage(TEXT("chooser.set_cell"), Text))
    {
        AddError(TEXT("Wiki renderer unavailable (no editor subsystem/dispatcher)"));
        return false;
    }

    TestFalse(TEXT("chooser.set_cell method page is not a Not-Found page"),
        Text.Contains(TEXT("# Not found:")));
    // The float-range cell shape must be discoverable from the method page itself.
    // "{min, max}" (with a space) is overlay-only; the param description uses the
    // spaceless "{min,max}", so this fails iff the overlay H3 is reverted.
    TestTrue(TEXT("set_cell page documents the float {min, max} value shape"),
        Text.Contains(TEXT("{min, max}")));
    // The object cell's assetPath alias note lives only in the overlay table.
    TestTrue(TEXT("set_cell page documents the object value/assetPath alias"),
        Text.Contains(TEXT("assetPath")));
    return true;
}
