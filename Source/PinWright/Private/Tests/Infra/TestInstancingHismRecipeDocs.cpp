// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for B-wiki-hism-recipe-resets-actor-transform.
//
// The owned-HISM recipe on level-building.instancing-and-scatter used to prescribe
// `a.set_editor_property('root_component', comp)`. That swaps the holder's root for a component at
// identity, and an actor's location is its root's location, so a layer built that way landed at the
// world origin. The page must keep the root (actor.add_component attaches under it), fill in local
// space with actor.add_instances, keep a location read-back that fails on the swap, and never put
// the root write back into executable code.
// Rendered through WikiHandler::RenderPage, the same path the generated wiki uses.
#include "Misc/AutomationTest.h"
#include "Tests/Infra/WikiDocTestHelpers.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FInstancingHismRecipeDocTest,
    "PinWright.infra.wiki_handler.WorkflowPage.InstancingHismRecipe",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FInstancingHismRecipeDocTest::RunTest(const FString& Parameters)
{
    FString Text;
    if (!WikiDocTestHelpers::RenderOrFail(*this, TEXT("level-building.instancing-and-scatter"), Text))
    {
        return false;
    }

    TestTrue(TEXT("recipe adds the HISM with actor.add_component"),
        Text.Contains(TEXT("call(\"actor.add_component\"")) &&
        Text.Contains(TEXT("componentType: \"HierarchicalInstancedStaticMeshComponent\"")));
    TestTrue(TEXT("recipe spawns the holder at its anchor through the editor factory"),
        Text.Contains(TEXT("spawn_actor_from_class(unreal.Actor, anchor)")));
    TestTrue(TEXT("recipe addresses the holder by the path step 1 prints, not by label"),
        Text.Contains(TEXT("print(a.get_path_name())")) &&
        Text.Contains(TEXT("actorName: holder")));
    TestTrue(TEXT("recipe fills with actor.add_instances in local space (the verb defaults to world)"),
        Text.Contains(TEXT("call(\"actor.add_instances\"")) &&
        Text.Contains(TEXT("space: \"local\"")));
    TestTrue(TEXT("recipe reads back the holder location, which is what fails on a root swap"),
        Text.Contains(TEXT("call(\"actor.get_transform\", { actorName: holder })")));
    TestFalse(TEXT("page no longer claims the root write leaves no undo record"),
        Text.Contains(TEXT("leaves no undo record")));
    TestTrue(TEXT("page warns that the root write moves the layer to the origin"),
        Text.Contains(TEXT("Never make the HISM the root")));

    // The root write may be named in prose as the thing not to do, never in a code block.
    TArray<FString> Lines;
    Text.ParseIntoArrayLines(Lines, false);
    bool bInCode = false;
    bool bRootWriteInCode = false;
    for (const FString& Line : Lines)
    {
        if (Line.TrimStartAndEnd().StartsWith(TEXT("```")))
        {
            bInCode = !bInCode;
            continue;
        }
        bRootWriteInCode |= bInCode && Line.Contains(TEXT("root_component"));
    }
    TestFalse(TEXT("no code block on the page writes root_component"), bRootWriteInCode);

    return true;
}
