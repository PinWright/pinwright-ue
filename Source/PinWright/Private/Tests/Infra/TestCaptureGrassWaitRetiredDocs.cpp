// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for B-capture-docs-prescribe-retired-grass-wait.
//
// Every level capture force-syncs the landscape-grass build for its own eye
// (LandscapeGrassSettle.cpp: RegenerateGrass(bInForceSync=true)), which skips the engine's
// grass.MaxCreatePerFrame budget. render.md and vegetation-authoring.md still prescribed the
// pre-settle ritual: editor.set_camera (moves the shared viewport camera), a 60-150 s wait, and
// repeat-until-stable captures. These tests fail if either page prescribes it again or stops
// pointing the caller at the grass block fields that answer the question.
#include "Misc/AutomationTest.h"
#include "Tests/Infra/WikiDocTestHelpers.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRenderGrassWaitRetiredDocTest,
    "PinWright.infra.wiki_handler.Namespace.RenderGrassWaitRetired",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FRenderGrassWaitRetiredDocTest::RunTest(const FString& Parameters)
{
    FString Text;
    if (!WikiDocTestHelpers::RenderOrFail(*this, TEXT("render"), Text))
    {
        return false;
    }

    TestFalse(TEXT("render page no longer prescribes set_camera + wait"),
        Text.Contains(TEXT("Move the camera with `editor.set_camera`, **wait**")));
    TestTrue(TEXT("render page says a force-synced build skips the per-frame cap"),
        Text.Contains(TEXT("a force-synced build is not subject to the per-frame cap")));
    TestTrue(TEXT("render page names builtForPose/settled/pendingComponents as the check"),
        Text.Contains(TEXT("`grass.builtForPose: true` with `grass.settled: true` and `grass.pendingComponents: 0`")));
    TestTrue(TEXT("render page makes grassWarning the only re-shoot trigger"),
        Text.Contains(TEXT("Re-shoot only when a `grassWarning` is present")));
    // The ortho burst's grass block is the last tile's; the burst verdict is the aggregate.
    TestTrue(TEXT("render page points ortho-tile callers at grass.allTilesSettled"),
        Text.Contains(TEXT("read `grass.allTilesSettled` for the burst")));
    // A culled-out-of-frame build reads built and settled yet carries a grassWarning.
    TestTrue(TEXT("render page's pairing rule also requires no grassWarning"),
        Text.Contains(TEXT("read that way and carry no `grassWarning` (or report `grass.landscapes: 0`)")));
    // The frame-settle warning is still true and must survive the rewrite.
    TestTrue(TEXT("render page keeps the warmup.settled-is-not-scene-readiness warning"),
        Text.Contains(TEXT("`warmup.settled` settles the *frame*, not the scene")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVegetationGrassWaitRetiredDocTest,
    "PinWright.infra.wiki_handler.WorkflowPage.VegetationGrassWaitRetired",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FVegetationGrassWaitRetiredDocTest::RunTest(const FString& Parameters)
{
    FString Text;
    if (!WikiDocTestHelpers::RenderOrFail(*this, TEXT("vegetation-authoring"), Text))
    {
        return false;
    }

    TestFalse(TEXT("vegetation page no longer says to set_camera before every capture"),
        Text.Contains(TEXT("Always `editor.set_camera` first")));
    TestFalse(TEXT("vegetation page no longer prescribes repeat-until-stable captures"),
        Text.Contains(TEXT("Capture repeatedly at the fixed pose until")));
    TestTrue(TEXT("vegetation page says not to wait for grass"),
        Text.Contains(TEXT("Do not wait for grass after a camera move")));
    TestTrue(TEXT("vegetation page says the capture build skips the per-frame cap"),
        Text.Contains(TEXT("own eye, which skips the per-frame cap")));
    TestTrue(TEXT("vegetation page points at grass.builtForPose"),
        Text.Contains(TEXT("Read `grass.builtForPose`")));
    TestTrue(TEXT("vegetation page's pairing rule also requires no grassWarning"),
        Text.Contains(TEXT("`grassWarning` (or report `grass.landscapes: 0`)")));
    return true;
}
