// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for E-configure-slot-behavior-behaviortype-undiscoverable:
// ai.configure_slot_behavior takes a `behaviorType` whose only schema/wiki
// description is the bare phrase "Type of behavior" (AIHandler.cpp registration),
// enumerating no accepted values, and never states that `activityTags` must be
// registered via gameplay_tags.add first. Both are hard preconditions the handler
// validates up front — an unknown behaviorType is rejected with INVALID_PARAMS +
// availableBehaviorTypes, and an unregistered tag with INVALID_PARAMS + droppedTags
// (AIHandler.cpp configure_slot_behavior) — so a from-scratch author hit two
// undocumented gates and learned both only by tripping the error / reading the C++.
// The fix adds a `### ai.configure_slot_behavior` H3 overlay section to
// docs/wiki-src/ai.md documenting the behaviorType accepted-value shape (concrete
// USmartObjectBehaviorDefinition subclass by /Script/Module.Class path, plugin-
// dependent loadable set), the availableBehaviorTypes-echo discovery move, and the
// gameplay_tags.add registration prereq for activityTags.
//
// This exercises the live WikiHandler::RenderPage render path (the same entry the
// HTTP gateway uses for doc requests), not a copy of the overlay text. Every marker
// asserted below is overlay-exclusive: the auto-generated method summary ("Configure
// behavior for a Smart Object slot") and the bare param descriptions ("Type of
// behavior", "Array of gameplay tag strings") name no accepted values, no
// availableBehaviorTypes discovery move, and no tag-registration prereq — so the H3
// section surfaces only when this method page is rendered directly, and reverting it
// makes LoadMethodSection return empty and these assertions fail.
#include "Misc/AutomationTest.h"
#include "Tests/Infra/WikiDocTestHelpers.h"

// ============================================================================
// Method page: the `### ai.configure_slot_behavior` H3 documents both undocumented
// gates — the behaviorType accepted-value shape + availableBehaviorTypes discovery
// move, and the activityTags gameplay_tags.add registration prereq. The H3 overlay
// surfaces only when the method page is rendered directly (not on the namespace
// page), so these markers live only in the H3 section of docs/wiki-src/ai.md.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSmartObjectSlotBehaviorDocTest,
    "PinWright.infra.wiki_handler.MethodPage.SmartObjectSlotBehaviorDiscoverability",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSmartObjectSlotBehaviorDocTest::RunTest(const FString& Parameters)
{
    FString Text;
    if (!WikiDocTestHelpers::RenderOrFail(*this, TEXT("ai.configure_slot_behavior"), Text))
    {
        return false;
    }

    // behaviorType accepted-value shape: a concrete USmartObjectBehaviorDefinition
    // subclass named by its /Script/Module.Class path — overlay-exclusive (the param
    // desc says only "Type of behavior").
    TestTrue(TEXT("configure_slot_behavior page states behaviorType is a concrete USmartObjectBehaviorDefinition subclass"),
        Text.Contains(TEXT("USmartObjectBehaviorDefinition")));
    TestTrue(TEXT("configure_slot_behavior page names a sensible default behaviorType path"),
        Text.Contains(TEXT("/Script/MassSmartObjects.SmartObjectMassBehaviorDefinition")));

    // The loadable set is plugin-dependent, not a fixed list (it is enumerated at
    // runtime from the linked behavior plugins).
    TestTrue(TEXT("configure_slot_behavior page states the loadable behaviorType set depends on linked plugins"),
        Text.Contains(TEXT("optional")) && Text.Contains(TEXT("plugin")));

    // The availableBehaviorTypes-echo discovery move: trip the error once and read
    // the accepted set back.
    TestTrue(TEXT("configure_slot_behavior page documents the availableBehaviorTypes discovery move"),
        Text.Contains(TEXT("availableBehaviorTypes")));

    // The activityTags registration prereq: tags must be registered via
    // gameplay_tags.add first or the call is rejected with droppedTags.
    TestTrue(TEXT("configure_slot_behavior page states activityTags must be registered via gameplay_tags.add"),
        Text.Contains(TEXT("gameplay_tags.add")));
    TestTrue(TEXT("configure_slot_behavior page states unregistered tags are rejected with droppedTags"),
        Text.Contains(TEXT("droppedTags")));
    return true;
}
