// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for E-inspect-namespace-methods-index-incomplete:
// The auto-generated `## Methods` index on a namespace page is built by Category
// (WikiHandler.cpp RenderMethodList -> MethodsByCategory[Reg.Category.ToLower()]),
// so a method appears in a namespace page's index only when its registered Category
// equals that namespace slug. Ten methods whose NAMES are `system.inspect.*`
// registered Category "system" in Handlers/Environment/EnvironmentHandler.cpp
// (genuine scene/inspection readers), while the 8 game-state/subsystem readers
// registered Category "system.inspect"
// (SystemInspectSingletonsHandler.cpp, SubsystemInspectHandler.cpp, ClassSearchHandler.cpp).
// Result before the fix: the `## Methods` index on the served `system.inspect` page
// listed only the 8 game-state readers, while the 10 `system.inspect.*` methods scattered
// onto the parent `system` page's index instead.
//
// The fix aligns Category with the method-name prefix: all 16 register Category
// "system.inspect". This test pins BOTH halves of that move on the live
// WikiHandler::RenderPage path (the same entry the HTTP gateway uses for doc requests):
//   (A) every `system.inspect.*` method now appears in the `system.inspect` page's
//       `## Methods` index (their name's page); and
//   (B) none of them still pollute the parent `system` page's `## Methods` index, while
//       the genuine `system.*` verbs remain there.
//
// The auto `## Methods` block is appended LAST by RenderPage (after the H1, the overlay
// prelude, the overlay `##` sections, and the `## Subgroups` index), so the index is
// isolated as the region from the final "## Methods" heading to end-of-page. That
// isolation matters: the overlay prose on both pages NAMES several of these methods
// (find_by_class, list_objects, ...), so a whole-page substring search
// would false-green. The assertions below only inspect the extracted auto index.
#include "Misc/AutomationTest.h"
#include "Tests/Infra/WikiDocTestHelpers.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSystemInspectMethodsIndexDocTest,
    "PinWright.infra.wiki_handler.Namespace.SystemInspectSceneReadersIndex",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSystemInspectMethodsIndexDocTest::RunTest(const FString& Parameters)
{
    // Isolate the auto-generated `## Methods` index (the last line-start "## Methods" heading
    // to EOF). Anchoring on a leading newline pins the match to a real H2 heading and can never
    // land on the "## Methods" substring inside a "### Methods..." overlay H3 heading; +1 drops
    // that anchor newline so the region still begins exactly at "## Methods".
    const auto ExtractMethodsIndex = [](const FString& Body) -> FString
    {
        const int32 Idx = Body.Find(TEXT("\n## Methods"), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
        return (Idx == INDEX_NONE) ? FString() : Body.Mid(Idx + 1);
    };

    // ---- (A) system.inspect page: every system.inspect.* method indexes on its own page ----
    FString InspectPage;
    if (!WikiDocTestHelpers::RenderOrFail(*this, TEXT("system.inspect"), InspectPage))
    {
        return false;
    }
    const FString InspectIndex = ExtractMethodsIndex(InspectPage);
    if (!TestTrue(TEXT("system.inspect page has an auto-generated `## Methods` index"),
            !InspectIndex.IsEmpty()))
    {
        return false;
    }

    // Positive control: a game-state reader always registered under Category
    // "system.inspect" IS listed. Proves the extracted region is the real, populated
    // method index (so the assertions below test the Category split, not an empty/
    // mis-extracted section). Passes both before and after the fix.
    TestTrue(TEXT("`## Methods` index lists the game-state reader system.inspect.list_subsystems"),
        InspectIndex.Contains(TEXT("system.inspect.list_subsystems")));

    // The fix: every method NAMED system.inspect.* that lives in EnvironmentHandler.cpp
    // now appears in the system.inspect `## Methods` index. Pre-fix each registered
    // Category "system" and was absent from this extracted region (they rendered on the
    // `system` page instead).
    const TCHAR* InspectMethods[] = {
        // Genuine scene / inspection readers, all registered Category "system.inspect".
        TEXT("system.inspect.get_viewport_info"),
        TEXT("system.inspect.get_selected_actors"),
        TEXT("system.inspect.list_objects"),
        TEXT("system.inspect.find_by_class"),
        TEXT("system.inspect.find_objects_by_class"),
        TEXT("system.inspect.find_by_tag"),
        TEXT("system.inspect.list_actor_tags"),
        TEXT("system.inspect.list_actor_classes"),
        TEXT("system.inspect.inspect_class"),
        TEXT("system.inspect.inspect_object"),
    };
    for (const TCHAR* Method : InspectMethods)
    {
        TestTrue(
            *FString::Printf(TEXT("system.inspect `## Methods` index lists %s"), Method),
            InspectIndex.Contains(Method));
    }

    // ---- (B) system page: the system.inspect.* methods no longer pollute the parent index; genuine verbs stay ----
    FString SystemPage;
    if (!WikiDocTestHelpers::RenderOrFail(*this, TEXT("system"), SystemPage))
    {
        return false;
    }
    const FString SystemIndex = ExtractMethodsIndex(SystemPage);
    if (!TestTrue(TEXT("system page has an auto-generated `## Methods` index"),
            !SystemIndex.IsEmpty()))
    {
        return false;
    }

    // Control + over-removal guard: a genuine Category-"system" verb still indexes here.
    TestTrue(TEXT("system `## Methods` index still lists the genuine verb system.run_ubt"),
        SystemIndex.Contains(TEXT("system.run_ubt")));

    // The declutter half of the fix: no method named system.inspect.* remains on the
    // parent `system` index — they all moved to their own page. Pre-fix this extracted
    // region carried them all (matched "system.inspect."); post-fix it carries none.
    // (The `## Subgroups` link `system.inspect` sits above `## Methods` and is excluded by
    // the extraction; it also lacks the trailing dot this substring requires.)
    TestFalse(TEXT("system `## Methods` index no longer lists any system.inspect.* method"),
        SystemIndex.Contains(TEXT("system.inspect.")));

    return true;
}
