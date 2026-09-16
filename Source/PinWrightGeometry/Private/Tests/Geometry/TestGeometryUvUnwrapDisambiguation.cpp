// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for E-geometry-auto-uv-redundant-with-unwrap-uv:
// geometry.auto_uv and geometry.unwrap_uv are the same XAtlas auto-unwrap op
// (AutoGenerateXAtlasMeshUVs). auto_uv used to (a) hardcode the UV channel to 0
// (no uvChannel param) and (b) carry a near-synonym summary that didn't point at
// unwrap_uv, so picking the channel-aware verb required comparison-reading both
// param tables. The fix converges them: auto_uv now takes the same uvChannel param
// and both summaries cross-reference the sibling as an alias. This guards that
// convergence against regression. It exercises the live registration records (the
// same data WikiHandler renders and the dispatcher serves), not a copy, so reverting
// either handler edit fails these assertions.
#include "Misc/AutomationTest.h"
#include "Tests/TestUtils.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryUvUnwrapDisambiguationTest,
    "PinWright.infra.geometry.AutoUvConvergesWithUnwrapUv",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGeometryUvUnwrapDisambiguationTest::RunTest(const FString& Parameters)
{
    TestTrue(TEXT("geometry.auto_uv registered"), IsHandlerRegistered(TEXT("geometry.auto_uv")));
    TestTrue(TEXT("geometry.unwrap_uv registered"), IsHandlerRegistered(TEXT("geometry.unwrap_uv")));

    // auto_uv must expose the same uvChannel param as unwrap_uv so it can target a
    // non-zero channel (it used to hardcode channel 0 with no param).
    TestNotNull(TEXT("geometry.auto_uv has a uvChannel param (no longer channel-0-frozen)"),
        GetRegisteredParamSpec(TEXT("geometry.auto_uv"), TEXT("uvChannel")));

    // Both summaries must cross-reference the sibling so the discovery surface
    // disambiguates the two near-synonym XAtlas verbs without comparison-reading
    // both param tables. GetRegisteredSummary returns "" for an unregistered method,
    // so these Contains() checks fail-closed.
    TestTrue(
        TEXT("geometry.auto_uv summary names unwrap_uv as its canonical sibling"),
        GetRegisteredSummary(TEXT("geometry.auto_uv")).ToLower().Contains(TEXT("unwrap_uv")));

    TestTrue(
        TEXT("geometry.unwrap_uv summary names auto_uv as its alias"),
        GetRegisteredSummary(TEXT("geometry.unwrap_uv")).ToLower().Contains(TEXT("auto_uv")));

    return true;
}
