// Copyright (c) 2026 Alexander Penkin. MIT License.

// Doc-sync guard for material-at-spawn on actor.spawn / actor.spawn_shape /
// actor.spawn_batch.
//
// The registered param glosses already render the words "materialPath", "materialPaths",
// "slot 0" and "warnings", so Text.Contains on those alone passes with the docs/wiki-src/
// actor.md overlay reverted. These assertions therefore pin only overlay-exclusive facts a
// caller cannot get from the param table: that the write lands on the mesh COMPONENT's
// OverrideMaterials (not the mesh asset, which is static_mesh.set_material's job), that a
// bad path is rejected BEFORE the spawn so no orphan actor is created, and - for
// spawn_batch - that a per-entry override replaces the batch default wholesale and that one
// bad path aborts the entire batch.
#include "Misc/AutomationTest.h"
#include "Tests/Infra/WikiDocTestHelpers.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSpawnMaterialDocTest,
    "PinWright.infra.wiki_handler.MethodPage.ActorSpawnMaterial",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSpawnMaterialDocTest::RunTest(const FString& Parameters)
{
    FString Text;
    if (!WikiDocTestHelpers::RenderOrFail(*this, TEXT("actor.spawn"), Text))
    {
        return false;
    }

    // Which store the write lands in - the component override, not the mesh asset.
    TestTrue(TEXT("actor.spawn page names OverrideMaterials on the component"),
        Text.Contains(TEXT("OverrideMaterials")));
    TestTrue(TEXT("actor.spawn page distinguishes the asset default from the instance override"),
        Text.Contains(TEXT("static_mesh.set_material")));

    // The pre-flight contract - overlay-only; the param gloss does not say "before".
    TestTrue(TEXT("actor.spawn page states a bad material path is raised before the spawn"),
        Text.Contains(TEXT("before")) && Text.Contains(TEXT("orphan")));

    // The graceful-degradation rule.
    TestTrue(TEXT("actor.spawn page states slot overflow / no mesh component are not errors"),
        Text.Contains(TEXT("no mesh component")));

    // The save-discipline pointer.
    TestTrue(TEXT("actor.spawn page points at the save step"),
        Text.Contains(TEXT("level.save")) || Text.Contains(TEXT("editor.save_all")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSpawnBatchMaterialDocTest,
    "PinWright.infra.wiki_handler.MethodPage.ActorSpawnBatchMaterial",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSpawnBatchMaterialDocTest::RunTest(const FString& Parameters)
{
    FString Text;
    if (!WikiDocTestHelpers::RenderOrFail(*this, TEXT("actor.spawn_batch"), Text))
    {
        return false;
    }

    // Per-entry override REPLACES the batch default; it is not merged slot-by-slot.
    TestTrue(TEXT("spawn_batch page states a per-entry override replaces the batch default"),
        Text.Contains(TEXT("replaces")) && Text.Contains(TEXT("not merged")));

    // The anti-half-batch guarantee - overlay-only.
    TestTrue(TEXT("spawn_batch page states one bad path spawns nothing"),
        Text.Contains(TEXT("nothing spawns")));
    TestTrue(TEXT("spawn_batch page names the single resolve pre-pass"),
        Text.Contains(TEXT("pre-pass")));

    // The existing skip reporting stays documented.
    TestTrue(TEXT("spawn_batch page documents the skipped array"),
        Text.Contains(TEXT("`skipped`")));
    return true;
}
