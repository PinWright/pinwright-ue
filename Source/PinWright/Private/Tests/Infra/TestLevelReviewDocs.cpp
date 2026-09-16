// Copyright (c) 2026 Alexander Penkin. MIT License.

// Keep the level review workflow aligned with the typed render and camera
// method contracts for axis-aligned orthographic capture.
#include "Misc/AutomationTest.h"
#include "Tests/Infra/WikiDocTestHelpers.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLevelReviewOrthoContractDocTest,
    "PinWright.infra.wiki_handler.WorkflowPage.LevelReviewOrthoContract",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLevelReviewOrthoContractDocTest::RunTest(const FString& Parameters)
{
    // The orthographic contract moved to the framing-math topic page when the
    // review page was split; the assertions follow the text rather than relax.
    FString Text;
    if (!WikiDocTestHelpers::RenderOrFail(*this, TEXT("level-review.framing-math"), Text))
    {
        return false;
    }

    TestTrue(TEXT("review documents raw capture rejection"),
        Text.Contains(TEXT("render.capture_open_level")) &&
        Text.Contains(TEXT("UNSUPPORTED_ORTHOGRAPHIC_ROTATION")) &&
        Text.Contains(TEXT("never silently captures")));
    TestTrue(TEXT("review documents camera helper snapping"),
        Text.Contains(TEXT("camera.frame_actor")) &&
        Text.Contains(TEXT("camera.orbit_shots")) &&
        Text.Contains(TEXT("orthoAxisSnapped")) &&
        Text.Contains(TEXT("requested and applied poses")));
    TestFalse(TEXT("review does not claim every surface snaps"),
        Text.Contains(TEXT("Every capture surface therefore")));

    return true;
}
