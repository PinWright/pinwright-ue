// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for E-asset-list-pagination-undocumented-spills:
// asset.list registers a `pagination` object (AssetManageHandler.cpp:693) whose
// offset/limit are honored (:728-736, :842-857) and the response already emits
// totalCount/count/offset (:891-897) — the cap genuinely works. The gap was
// purely discoverability: the `### asset.list` overlay in docs/wiki-src/asset.md
// documented only the filter.class table and the top-level `path`, never the
// pagination lever or the totalCount/count readback, while the two sibling list
// verbs right below it (asset.search, asset.search_assets) each document their
// `limit`. So the natural unpaginated call overflowed the inline budget and
// spilled to the HttpResponses file.
//
// The fix extends the existing `### asset.list` H3 to document the `pagination`
// object, the totalCount/count/offset readback, and the spill/keep-it-inline
// guidance, mirroring the sibling sections. This test renders the live method
// page through WikiHandler::RenderPage (the same entry the HTTP gateway uses for
// doc requests), not a copy of the overlay text. Reverting the overlay makes
// LoadMethodSection return empty and these assertions fail.
//
// Marker selection: the bare param names (`pagination`, `offset`, `limit`) and
// the registered gloss "Pagination with offset and limit" are NOT
// overlay-exclusive — RenderMethodPage emits the auto-generated param list
// (WikiHandler.cpp:416) before appending the overlay H3 under `## Notes`
// (:418-424), so Text.Contains on those words passes with the overlay reverted.
// This test therefore asserts only on distinctive overlay-only content: the
// `totalCount` readback field (NOT a param name — only the overlay names it), the
// elision-detection phrasing, the spill-to-HttpResponses note, and the
// keep-it-inline guidance.
#include "Misc/AutomationTest.h"
#include "Tests/Infra/WikiDocTestHelpers.h"

// ============================================================================
// Method page: `### asset.list` H3 documents the pagination lever (the
// totalCount/count readback as the elision signal) and the spill / keep-inline
// guidance that steers a caller to pagination.limit before the response spills.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetListPaginationDocTest,
    "PinWright.infra.wiki_handler.MethodPage.AssetListPagination",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetListPaginationDocTest::RunTest(const FString& Parameters)
{
    FString Text;
    if (!WikiDocTestHelpers::RenderOrFail(*this, TEXT("asset.list"), Text))
    {
        return false;
    }

    // The readback contract: totalCount is the full match count and a
    // count < totalCount readback is the truncation signal. The
    // `count < totalCount` phrasing is overlay-exclusive — `count` alone is a
    // case-insensitive substring of `totalCount` (and neither response field is a
    // registered param the auto-generated param list emits), so only this whole
    // phrase proves both fields *and* their elision relationship are documented.
    TestTrue(TEXT("asset.list page documents the count < totalCount elision-signal readback"),
        Text.Contains(TEXT("count < totalCount")));

    // The spill consequence the ticket flags: an unpaginated list overflows the
    // inline budget and spills to the HttpResponses file. Overlay-only — the
    // param gloss ("Pagination with offset and limit") says nothing of this.
    TestTrue(TEXT("asset.list page notes the unpaginated list overflows the inline budget and spills"),
        Text.Contains(TEXT("HttpResponses")) || Text.Contains(TEXT("outputTooLong")));

    // The steering guidance: pass pagination.limit up front to keep the listing
    // inline. Overlay-only distinctive phrasing.
    TestTrue(TEXT("asset.list page steers callers to pass limit to keep the listing inline"),
        Text.Contains(TEXT("inline")) && Text.Contains(TEXT("limit")));

    return true;
}
