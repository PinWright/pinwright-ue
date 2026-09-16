// Copyright (c) 2026 Alexander Penkin. MIT License.

// Guard the level-building hub against stale summaries that contradict the typed
// method pages agents follow while authoring a level.
#include "Misc/AutomationTest.h"
#include "Tests/Infra/WikiDocTestHelpers.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLevelBuildingHubContractsDocTest,
    "PinWright.infra.wiki_handler.WorkflowPage.LevelBuildingContracts",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLevelBuildingHubContractsDocTest::RunTest(const FString& Parameters)
{
    FString Text;
    if (!WikiDocTestHelpers::RenderOrFail(*this, TEXT("level-building"), Text))
    {
        return false;
    }

    TestTrue(TEXT("hub documents material-at-spawn"),
        Text.Contains(TEXT("Spawn verbs can assign materials")) &&
        Text.Contains(TEXT("materialPath")) && Text.Contains(TEXT("materialPaths")));
    TestTrue(TEXT("hub documents missing batch locations as origin placements"),
        Text.Contains(TEXT("omits `location`")) && Text.Contains(TEXT("world origin")) &&
        Text.Contains(TEXT("not dropped")));
    TestTrue(TEXT("hub distinguishes create loss from overwrite preservation"),
        Text.Contains(TEXT("On the **create** path")) && Text.Contains(TEXT("materials are not carried")) &&
        Text.Contains(TEXT("overwrite:true")) && Text.Contains(TEXT("existing collision remain preserved")));

    // The capture guidance was extracted to its own topic page when the hub was
    // split; the assertion follows the text rather than being relaxed.
    FString CaptureText;
    if (!WikiDocTestHelpers::RenderOrFail(*this, TEXT("level-building.capture-and-review"), CaptureText))
    {
        return false;
    }

    TestTrue(TEXT("capture page distinguishes raw ortho rejection from camera snapping"),
        CaptureText.Contains(TEXT("render.capture_open_level")) &&
        CaptureText.Contains(TEXT("UNSUPPORTED_ORTHOGRAPHIC_ROTATION")) &&
        CaptureText.Contains(TEXT("camera.orbit_shots")) && CaptureText.Contains(TEXT("do snap")));

    return true;
}
