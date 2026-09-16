// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for E-container-set-no-discoverable-target: the container.set /
// container.map / container.array wiki overlays named no concrete container UPROPERTY
// to operate on and gave no recipe for finding one, so a caller had to hunt for a
// target (grepping engine source for a TSet/TMap UPROPERTY) before issuing a single
// container.* call. The fix adds the container-kind-agnostic discovery recipe (the
// property.list -> cppType scan plus the "no container-kind filter" caveat) ONCE to the
// parent docs/wiki-src/container.md overlay (## Cross-cluster overlap), and adds a
// `## Finding a <T> target` namespace section to each of container.set.md /
// container.map.md / container.array.md naming an always-present engine-CDO target that
// links back to the shared recipe.
//
// These sections live in the overlay PRELUDE (everything before the first `### ` line)
// for the child pages and in a `## ` section on container.md, so they render on the
// namespace page (container / container.set / container.map / container.array) via
// WikiHandler::RenderPage -> RenderNamespaceHeader -> WikiOverlay::LoadGroupPrelude.
// This exercises the live render path the HTTP gateway uses for doc requests, not a copy
// of the overlay text. Every marker asserted below is overlay-exclusive (the auto Methods
// index and the per-verb auto summaries name no target object and no cppType recipe), so
// reverting the overlay sections makes LoadGroupPrelude return only the original
// descriptions and these assertions fail.
#include "Misc/AutomationTest.h"
#include "Tests/Infra/WikiDocTestHelpers.h"

// ============================================================================
// container parent page: the `## Cross-cluster overlap` section carries the shared,
// container-kind-agnostic discovery recipe (property.list -> cppType scan) and the
// "property.list has no cppType/container-kind filter" caveat — written once, not per
// child page.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FContainerTargetDiscoveryRecipeDocTest,
    "PinWright.infra.wiki_handler.Namespace.ContainerTargetDiscoveryRecipe",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FContainerTargetDiscoveryRecipeDocTest::RunTest(const FString& Parameters)
{
    FString Text;
    if (!WikiDocTestHelpers::RenderOrFail(*this, TEXT("container"), Text))
    {
        return false;
    }

    // The shared "finding a container target" guidance must exist on the parent page.
    TestTrue(TEXT("container page carries a 'Finding a container target' bullet"),
        Text.Contains(TEXT("Finding a container target")));
    // The discovery recipe + the no-container-kind-filter caveat live here, once.
    WikiDocTestHelpers::AssertDiscoveryRecipe(*this, TEXT("container page"), Text);
    return true;
}

// ============================================================================
// container.set namespace page: the `## Finding a TSet target` section names the
// AssetManagerSettings CDO TSet<FName> target. The shared recipe is asserted against
// the parent page above, not re-asserted here.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FContainerSetTargetDiscoveryDocTest,
    "PinWright.infra.wiki_handler.Namespace.ContainerSetTargetDiscovery",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FContainerSetTargetDiscoveryDocTest::RunTest(const FString& Parameters)
{
    FString Text;
    if (!WikiDocTestHelpers::RenderOrFail(*this, TEXT("container.set"), Text))
    {
        return false;
    }

    // The "finding a target" section must exist on the namespace page.
    TestTrue(TEXT("container.set page carries a 'Finding a TSet target' section"),
        Text.Contains(TEXT("Finding a TSet target")));
    // The always-present copy-paste CDO target (AssetManagerSettings.MetaDataTagsForAssetRegistry).
    TestTrue(TEXT("container.set page names the AssetManagerSettings CDO objectPath"),
        Text.Contains(TEXT("/Script/Engine.Default__AssetManagerSettings")));
    TestTrue(TEXT("container.set page names the MetaDataTagsForAssetRegistry TSet<FName> property"),
        Text.Contains(TEXT("MetaDataTagsForAssetRegistry")));
    return true;
}

// ============================================================================
// container.map namespace page: the `## Finding a TMap target` section names an
// always-present TMap CDO target. The shared recipe is asserted against the parent page.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FContainerMapTargetDiscoveryDocTest,
    "PinWright.infra.wiki_handler.Namespace.ContainerMapTargetDiscovery",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FContainerMapTargetDiscoveryDocTest::RunTest(const FString& Parameters)
{
    FString Text;
    if (!WikiDocTestHelpers::RenderOrFail(*this, TEXT("container.map"), Text))
    {
        return false;
    }

    TestTrue(TEXT("container.map page carries a 'Finding a TMap target' section"),
        Text.Contains(TEXT("Finding a TMap target")));
    // The always-present copy-paste CDO target (UserInterfaceSettings.HardwareCursors).
    TestTrue(TEXT("container.map page names the UserInterfaceSettings CDO objectPath"),
        Text.Contains(TEXT("/Script/Engine.Default__UserInterfaceSettings")));
    TestTrue(TEXT("container.map page names the HardwareCursors TMap property"),
        Text.Contains(TEXT("HardwareCursors")));
    return true;
}

// ============================================================================
// container.array namespace page: the `## Finding a TArray target` section names an
// always-present TArray CDO target. The shared recipe is asserted against the parent page.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FContainerArrayTargetDiscoveryDocTest,
    "PinWright.infra.wiki_handler.Namespace.ContainerArrayTargetDiscovery",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FContainerArrayTargetDiscoveryDocTest::RunTest(const FString& Parameters)
{
    FString Text;
    if (!WikiDocTestHelpers::RenderOrFail(*this, TEXT("container.array"), Text))
    {
        return false;
    }

    TestTrue(TEXT("container.array page carries a 'Finding a TArray target' section"),
        Text.Contains(TEXT("Finding a TArray target")));
    // The always-present copy-paste CDO target (AssetManagerSettings.DirectoriesToExclude).
    TestTrue(TEXT("container.array page names the AssetManagerSettings CDO objectPath"),
        Text.Contains(TEXT("/Script/Engine.Default__AssetManagerSettings")));
    TestTrue(TEXT("container.array page names the DirectoriesToExclude TArray property"),
        Text.Contains(TEXT("DirectoriesToExclude")));
    return true;
}
